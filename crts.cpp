#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <math.h>
#include <complex>
#include <liquid/liquid.h>
// Definition of liquid_float_complex changes depending on
// whether <complex> is included before or after liquid.h
#include <liquid/ofdmtxrx.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
// For Threading (POSIX Threads)
#include <pthread.h>
// For config file
#include <libconfig.h>

//TCP Header Files
#include <sys/socket.h> // for socket(), connect(), send(), and recv() 
#include <sys/types.h>
#include <arpa/inet.h>  // for sockaddr_in and inet_addr() 
#include <string.h>     // for memset() 
#include <unistd.h>     // for close() 
#include <errno.h>
#include <sys/types.h>  // for killing child process
#include <signal.h>     // for killing child process
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/msg.hpp>
#include <getopt.h>     // For command line options
#define MAXPENDING 5

// SO_REUSEPORT is defined only defined with linux 3.10+.
// Makes compatible with earlier kernels.
#ifndef SO_REUSEPORT
#define SO_REUSEPORT SO_REUSEADDR
#endif

void usage() {
    printf("crts -- Test cognitive radio engines. Data is logged in the 'data' directory to a file named 'data_crts' with date and time appended.\n");
    printf("  -u,-h  :   usage/help\n");
    printf("  -q     :   quiet - do not print debug info\n");
    printf("  -v     :   verbose - print debug info to stdout (default)\n");
    printf("  -d     :   print data to stdout rather than to file (implies -q unless -v given)\n");
    printf("  -r     :   real transmissions using USRPs (opposite of -s)\n");
    printf("  -s     :   simulation mode (default)\n");
    printf("  -p     :   server port (default: 1402)\n");
    printf("  -c     :   controller - this crts instance will act as experiment controller (needs -r)\n");
    printf("  -a     :   server IP address (when not controller. default: 127.0.0.1)\n");
    printf("  -f     :   center frequency [Hz] (when not controller. default: 460 MHz)\n");
    printf("  -b     :   bandwidth [Hz], (when not controller. default: 1.0 MHz)\n");
    printf("  -G     :   uhd rx gain [dB] (when not controller. default: 20dB)\n");
    printf("  -M     :   number of subcarriers (when not controller. default: 64)\n");
    printf("  -C     :   cyclic prefix length (when not controller. default: 16)\n");
    printf("  -T     :   taper length (when not controller. default: 4)\n");
    //printf("  f     :   center frequency [Hz], default: 462 MHz\n");
    //printf("  b     :   bandwidth [Hz], default: 250 kHz\n");
    //printf("  G     :   uhd rx gain [dB] (default: 20dB)\n");
    //printf("  t     :   run time [seconds]\n");
    //printf("  z     :   number of subcarriers to notch in the center band, default: 0\n");
}

// Class to create running average objects
template <class T>
class running_avg{
	public:
	// Variables
	int avg_len;
	int position;
	float avg;
	T *memory;

	// Constructor
	running_avg(int length){
		avg_len = length;
		position = 0;
		avg = (T) 0;
		memory = new T[avg_len]();
	}

	// Destructor
	~running_avg(){
		delete memory;
	}

	// Function to update the running average
	float update(T input){
		avg -= (float)*(memory+position)/(float)avg_len;
		*(memory+position) = input;
		avg += (float)input/(float)avg_len;
		position++;
		if(position == avg_len) position = 0;
		return avg;
	}
};

struct CognitiveEngine {
    // Modulation/coding parameters
	char modScheme[30];
    char crcScheme[30];
    char innerFEC[30];
    char outerFEC[30];
    char outerFEC_prev[30];
	unsigned int bitsPerSym;
	unsigned int numSubcarriers;
    unsigned int CPLen;
    unsigned int taperLen;
	unsigned int payloadLen;

	// RF parameters
	float default_tx_power;
	float txgain_dB;
    float uhd_txgain_dB;
	float uhd_rxgain_dB;
    float frequency_tx;
	float frequency_rx;
    float bandwidth;

  	// Cognitive parameters
    // TODO: For latestGoalValue, Use different type of variable depending on
    //  what its being compared to
    char adaptationCondition[30];
    char adaptation[30];
    char goal[30];
	int goal_averaging;	
	float goal_mem[100];
	float averagedGoalValue;
    float threshold;
	float latestGoalValue;
    float weightedAvg; 
    float weighted_avg_payload_valid_threshold;
    float PER_threshold;
    float BER_threshold;
    float FECswitch;
    unsigned int payloadLenIncrement;
    unsigned int payloadLenMax;
    unsigned int payloadLenMin;
   	
	// Control variables
	float delay_us;
    double startTime;
    double runningTime;
    int iteration;
	unsigned int frameNumber;

	// Performance metrics
    float PER;
    float PER_avg;
	int PER_averaging;
	running_avg<float> *PER_RA_ptr;

	float BER;
	float BER_avg;
	int BER_averaging;
	running_avg<float> *BER_RA_ptr;

	unsigned int validPayloads;
    float validPayloads_avg;
	int validPayloads_averaging;
	running_avg<float> *validPayloads_RA_ptr;
	
	unsigned int errorFreePayloads;
	float errorFreePayloads_avg;
	int errorFreePayloads_averaging;
	running_avg<float> *errorFreePayloads_RA_ptr;
	
    unsigned int lastReceivedFrame;
};

struct Scenario {
    int addAWGNBasebandTx; //Does the Scenario have noise?
    int addAWGNBasebandRx; //Does the Scenario have noise?
    float noiseSNR;
    float noiseDPhi;
    
    int addInterference; // Does the Scenario have interference?
    
    int addRicianFadingBasebandTx; // Does the Secenario have fading?
    int addRicianFadingBasebandRx; // Does the Secenario have fading?
    float fadeK;
    float fadeFd;
    float fadeDPhi;

	int addCWInterfererBasebandTx; // Does the Scenario have a CW interferer?
	int addCWInterfererBasebandRx; // Does the Scenario have a CW interferer?
	float cw_pow;
	float cw_freq;
};

struct rxCBstruct {
    unsigned int serverPort;
    int verbose;
    float bandwidth;
    char * serverAddr;
	msequence * rx_ms_ptr;
    int frameNum;
	int client;
	int isController;
	int usingUSRPs;
	ofdmtxrx * txrx_ptr;
	struct CognitiveEngine * ce_ptr;
	struct Scenario * sc_ptr;
	struct feedbackStruct *fb_ptr;
};

struct feedbackStruct {
    int             header_valid;
    int             payload_valid;
    unsigned int    payload_len;
    unsigned int    payloadByteErrors;
    unsigned int    payloadBitErrors;
    unsigned int    iteration;
    float           evm;
    float           rssi;
    float           cfo;
	int 			block_flag;
    pthread_mutex_t fb_mutex;
    pthread_cond_t fb_cond;
};

struct serverThreadStruct {
    unsigned int serverPort;
	int OTA;
	int usingUSRPs;
    struct feedbackStruct * fb_ptr;
    int * client_ptr;
};

struct serveClientStruct {
	int client;
	int OTA;
	int usingUSRPs;
	struct feedbackStruct * fb_ptr;
};

struct enactScenarioBasebandRxStruct {
    ofdmtxrx * txcvr_ptr;
    struct CognitiveEngine * ce_ptr;
    struct Scenario * sc_ptr;
};

struct scenarioSummaryInfo{
	int total_frames[60][60];
	int valid_headers[60][60];
	int valid_payloads[60][60];
	int total_bits[60][60];
	int bit_errors[60][60];
	float EVM[60][60];
	float RSSI[60][60];
	float PER[60][60];
};

struct cognitiveEngineSummaryInfo{
	int total_frames[60];
	int valid_headers[60];
	int valid_payloads[60];
	int total_bits[60];
	int bit_errors[60];
	float EVM[60];
	float RSSI[60];
	float PER[60];
};

// Default parameters for a Cognitive Engine
struct CognitiveEngine CreateCognitiveEngine() {
    struct CognitiveEngine ce = {};

	// Modulation/coding parameters
    strcpy(ce.modScheme, "QPSK");
    strcpy(ce.crcScheme, "none");
    strcpy(ce.innerFEC, "none");
    strcpy(ce.outerFEC, "Hamming74");
    strcpy(ce.outerFEC_prev, "Hamming74");
	ce.bitsPerSym = 1;
	ce.numSubcarriers = 64;
    ce.CPLen = 16;         
    ce.taperLen = 4;       
    ce.payloadLen = 120;       
   
    // RF parameters
	ce.default_tx_power = 10.0;
	ce.txgain_dB = -12.0;
    ce.uhd_txgain_dB = 25.0;
	ce.bandwidth = 1.0e6;
    
	// Cognitive parameters
	strcpy(ce.adaptationCondition, "packet_error_rate"); 
    strcpy(ce.adaptation, "mod_scheme->BPSK");
    strcpy(ce.goal, "payload_valid");
    ce.goal_averaging = 1;
	ce.averagedGoalValue = 0;
	ce.threshold = 1.0;        
    ce.latestGoalValue = 0.0;  
    ce.weightedAvg = 0.0;
    ce.PER_threshold = 0.5;
    ce.BER_threshold = 0.5;
    ce.FECswitch = 1;
	ce.payloadLenIncrement = 2;
    ce.payloadLenMax = 500;
    ce.payloadLenMin = 20; 
    ce.weighted_avg_payload_valid_threshold = 0.5;
    
	// Control variables
	ce.delay_us = 1000000.0;
    ce.startTime = 0.0;
    ce.runningTime = 0.0; 
    ce.iteration = 1;          
    ce.frameNumber = 0;
    
	// Performance metrics
    ce.PER = 0.0;
    ce.PER_avg = 0.0;
	ce.PER_averaging = 1;

	ce.BER = 0.0;
    ce.BER_avg = 0.0;
	ce.BER_averaging =1;

    ce.validPayloads = 0;
    ce.validPayloads_avg = 0;
	ce.validPayloads_averaging = 1;
	
	ce.errorFreePayloads = 0;
	ce.errorFreePayloads = 0;
	ce.errorFreePayloads_averaging = 1;
    
	//memset(ce.metric_mem,0,100*sizeof(float));
	 
    ce.lastReceivedFrame = 0;
    
	return ce;
} // End CreateCognitiveEngine()

// Default parameter for Scenario
struct Scenario CreateScenario() {
    struct Scenario sc = {};
    sc.addAWGNBasebandTx = 0,
    sc.addAWGNBasebandRx = 0,
    sc.noiseSNR = 7.0f, // in dB
    sc.noiseDPhi = 0.001f,

    sc.addInterference = 0,

    sc.addRicianFadingBasebandTx = 0,
    sc.addRicianFadingBasebandRx = 0,
    sc.fadeK = 30.0f,
    sc.fadeFd = 0.2f,
    sc.fadeDPhi = 0.001f;

	sc.addCWInterfererBasebandTx = 0;
	sc.addCWInterfererBasebandRx = 0;
	sc.cw_pow = 0;
	sc.cw_freq = 0;

    return sc;
} // End CreateScenario()

// Defaults for struct that is sent to rxCallback()
struct rxCBstruct CreaterxCBStruct() {
    struct rxCBstruct rxCB = {};
    rxCB.serverPort = 1402;
    rxCB.bandwidth = 1.0e6;
    rxCB.serverAddr = (char*) "127.0.0.1";
    rxCB.verbose = 1;
    return rxCB;
} // End CreaterxCBStruct()

// Defaults for struct that is sent to server thread
struct serverThreadStruct CreateServerStruct() {
    struct serverThreadStruct ss = {};
    ss.serverPort = 1402;
    ss.fb_ptr = NULL;
    return ss;
} // End CreateServerStruct()

//Defaults for struct that is sent to client threads
struct serveClientStruct CreateServeClientStruct() {
	struct serveClientStruct sc = {};
	sc.client = 0;
	sc.fb_ptr = NULL;
	return sc;
}; // End CreateServeClientStruct

void feedbackStruct_print(feedbackStruct * fb_ptr)
{
    // TODO: make formatting nicer
    printf("feedbackStruct_print():\n");
    printf("\theader_valid:\t%d\n",       fb_ptr->header_valid);
    printf("\tpayload_valid:\t%d\n",      fb_ptr->header_valid);
    printf("\tpayload_len:\t%u\n",        fb_ptr->payload_len);
    printf("\tpayloadByteErrors:\t%u\n",  fb_ptr->payloadByteErrors);
    printf("\tpayloadBitErrors:\t%u\n",   fb_ptr->payloadBitErrors);
    printf("\tframeNum:\t%u\n",           fb_ptr->iteration);
    printf("\tevm:\t%f\n",                fb_ptr->evm);
    printf("\trssi:\t%f\n",               fb_ptr->rssi);
    printf("\tcfo:\t%f\n",                fb_ptr->cfo);
}

int readScMasterFile(char scenario_list[30][60], int verbose )
{
    config_t cfg;                   // Returns all parameters in this structure 
    config_setting_t *setting;
    const char *str;                // Stores the value of the String Parameters in Config file
    int tmpI;                       // Stores the value of Integer Parameters from Config file                

    char current_sc[30];
    int no_of_scenarios=1;
    int i;
    char tmpS[30];
    //Initialization
    config_init(&cfg);
   
   
    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,"master_scenario_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        fprintf(stderr, "\nCould not find master scenario file. It should be named 'master_scenario_file.txt'\n");
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }
  
    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        
        if (config_setting_lookup_int(setting, "NumberofScenarios", &tmpI))
        {
            no_of_scenarios=tmpI;
            if (verbose)
                printf ("Number of Scenarios: %d\n",tmpI);
        }
        
      	for (i=1;i<=no_of_scenarios;i++)
       	{
         	strcpy (current_sc,"scenario_");
         	sprintf (tmpS,"%d",i);
         	strcat (current_sc,tmpS);
       		if (config_setting_lookup_string(setting, current_sc, &str))
          	{
              	strcpy(*((scenario_list)+i-1),str);          
          	}
        	if (verbose)
            	printf ("Scenario File: %s\n", *((scenario_list)+i-1) );
      	} 
    }
    config_destroy(&cfg);
    return no_of_scenarios;
} // End readScMasterFile()

int readCEMasterFile(char cogengine_list[30][60], int verbose, int isController)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    const char *str;            // Stores the value of the String Parameters in Config file
    int tmpI;                   // Stores the value of Integer Parameters from Config file             

    char current_ce[30];
    int no_of_cogengines=1;
    int i;
    char tmpS[30];
	char type[12];
    
	//Initialization
    config_init(&cfg);

	if(isController) strcpy(type,"_controller");
	else strcpy(type,"_slave");
   
    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,"master_cogengine_file.txt"))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        fprintf(stderr, "\nCould not find master file. It should be named 'master_cogengine_file.txt'\n");
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }

    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        if (config_setting_lookup_int(setting, "NumberofCogEngines", &tmpI))
        {
            no_of_cogengines=tmpI;
            if (verbose)
                printf ("Number of Congnitive Engines: %d\n",tmpI);
        }
        
        for (i=1;i<=no_of_cogengines;i++)
        {
            strcpy (current_ce,"cogengine_");
            sprintf (tmpS,"%d",i);
            strcat (current_ce,tmpS);
			strcat(current_ce,type);

            if (config_setting_lookup_string(setting, current_ce, &str))
            {
                strcpy(*((cogengine_list)+i-1),str);          
            }
            if (verbose)
                printf ("Cognitive Engine File: %s\n", *((cogengine_list)+i-1) );
        } 
    }
    config_destroy(&cfg);
    return no_of_cogengines;
} // End readCEMasterFile()

///////////////////Cognitive Engine///////////////////////////////////////////////////////////
////////Reading the cognitive radio parameters from the configuration file////////////////////
int readCEConfigFile(struct CognitiveEngine * ce, char *current_cogengine_file, int verbose)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    const char * str;           // Stores the value of the String Parameters in Config file
    int tmpI;                   // Stores the value of Integer Parameters from Config file
    double tmpD;                
    char ceFileLocation[60];

    strcpy(ceFileLocation, "ceconfigs/");
    strcat(ceFileLocation, current_cogengine_file);

    if (verbose)
        printf("Reading ceconfigs/%s\n", current_cogengine_file);

    //Initialization
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg,ceFileLocation))
    {
        fprintf(stderr, "\n%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }

    // Read the parameter group
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the strings
        if (config_setting_lookup_string(setting, "adaptation", &str))
        {
            strcpy(ce->adaptation,str);
            if (verbose) printf("Option to adapt: %s\n",str);
        }
       
        if (config_setting_lookup_string(setting, "goal", &str))
        {
            strcpy(ce->goal,str);
            if (verbose) printf("Goal: %s\n",str);
        }
        if (config_setting_lookup_string(setting, "adaptationCondition", &str))
        {
            strcpy(ce->adaptationCondition,str);
            if (verbose) printf("adaptationCondition: %s\n",str);
        }
        if (config_setting_lookup_string(setting, "modScheme", &str))
        {
            strcpy(ce->modScheme,str);
            if (verbose) printf("Modulation Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "crcScheme", &str))
        {
            strcpy(ce->crcScheme,str);
            if (verbose) printf("CRC Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "innerFEC", &str))
        {
            strcpy(ce->innerFEC,str);
            if (verbose) printf("Inner FEC Scheme:%s\n",str);
        }
        if (config_setting_lookup_string(setting, "outerFEC", &str))
        {
            strcpy(ce->outerFEC,str);
            if (verbose) printf("Outer FEC Scheme:%s\n",str);
        }

        // Read the integers
        if (config_setting_lookup_int(setting, "iterations", &tmpI))
        {
           //ce->iteration=tmpI;
           if (verbose) printf("Iterations: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLen", &tmpI))
        {
           ce->payloadLen=tmpI; 
           if (verbose) printf("PayloadLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenIncrement", &tmpI))
        {
           ce->payloadLenIncrement=tmpI; 
           if (verbose) printf("PayloadLenIncrement: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenMax", &tmpI))
        {
           ce->payloadLenMax=tmpI; 
           if (verbose) printf("PayloadLenMax: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "payloadLenMin", &tmpI))
        {
           ce->payloadLenMin=tmpI; 
           if (verbose) printf("PayloadLenMin: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "numSubcarriers", &tmpI))
        {
           ce->numSubcarriers=tmpI; 
           if (verbose) printf("Number of Subcarriers: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "CPLen", &tmpI))
        {
           ce->CPLen=tmpI; 
           if (verbose) printf("CPLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "taperLen", &tmpI))
        {
           ce->taperLen=tmpI; 
           if (verbose) printf("taperLen: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "delay_us", &tmpI))
        {
           ce->delay_us=tmpI; 
           if (verbose) printf("delay_us: %d\n", tmpI);
        }
        // Read the floats
        if (config_setting_lookup_float(setting, "default_tx_power", &tmpD))
        {
           ce->default_tx_power=tmpD; 
           if (verbose) printf("Default Tx Power: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "latestGoalValue", &tmpD))
        {
           ce->latestGoalValue=tmpD; 
           if (verbose) printf("Latest Goal Value: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "threshold", &tmpD))
        {
           ce->threshold=tmpD; 
           if (verbose) printf("Threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "frequency_tx", &tmpD))
        {
           ce->frequency_tx=tmpD; 
           if (verbose) printf("Transmit frequency: %f\n", tmpD);
        }
		if (config_setting_lookup_float(setting, "frequency_rx", &tmpD))
        {
           ce->frequency_rx=tmpD; 
           if (verbose) printf("Receive frequency: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "txgain_dB", &tmpD))
        {
           ce->txgain_dB=tmpD; 
           if (verbose) printf("txgain_dB: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "bandwidth", &tmpD))
        {
           ce->bandwidth=tmpD; 
           if (verbose) printf("bandwidth: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "uhd_txgain_dB", &tmpD))
        {
           ce->uhd_txgain_dB=tmpD; 
           if (verbose) printf("uhd_txgain_dB: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "weighted_avg_payload_valid_threshold", &tmpD))
        {
           ce->weighted_avg_payload_valid_threshold=tmpD; 
           if (verbose) printf("weighted_avg_payload_valid_threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "PER_threshold", &tmpD))
        {
           ce->PER_threshold=tmpD; 
           if (verbose) printf("PER_threshold: %f\n", tmpD);
        }
        if (config_setting_lookup_float(setting, "BER_threshold", &tmpD))
        {
           ce->BER_threshold=tmpD; 
           if (verbose) printf("BER_threshold: %f\n", tmpD);
        }
		if (config_setting_lookup_float(setting, "goal_averaging", &tmpD))
        {
           ce->goal_averaging=tmpD; 
           if (verbose) printf("Goal averaging: %f\n", tmpD);
        }
		if (config_setting_lookup_int(setting, "BER_averaging", &tmpI))
        {
           ce->BER_averaging=tmpI;
		   ce->BER_RA_ptr = new running_avg<float>(tmpI);
           if (verbose) printf("BER averaging: %i\n", tmpI);
        }
		if (config_setting_lookup_int(setting, "PER_averaging", &tmpI))
        {
           ce->PER_averaging=tmpI;
		   ce->PER_RA_ptr = new running_avg<float>(tmpI);
           if (verbose) printf("PER averaging: %i\n", tmpI);
        }
		if (config_setting_lookup_int(setting, "validPayloads_averaging", &tmpI))
        {
           ce->validPayloads_averaging=tmpI;
		   ce->validPayloads_RA_ptr = new running_avg<float>(tmpI);
           if (verbose) printf("Valid payloads averaging: %i\n", tmpI);
        }
		if (config_setting_lookup_int(setting, "errorFreePayloads_averaging", &tmpI))
        {
           ce->errorFreePayloads_averaging=tmpI;
		   ce->errorFreePayloads_RA_ptr = new running_avg<float>(tmpI);
           if (verbose) printf("BER averaging: %i\n", tmpI);
        }

	
    }
    config_destroy(&cfg);
    return 1;
} // End readCEConfigFile()

int readScConfigFile(struct Scenario * sc, char *current_scenario_file, int verbose)
{
    config_t cfg;               // Returns all parameters in this structure 
    config_setting_t *setting;
    //const char * str;
    int tmpI;
    double tmpD;
    char scFileLocation[60];

    // Because the file is in the folder 'scconfigs'
    strcpy(scFileLocation, "scconfigs/");
    strcat(scFileLocation, current_scenario_file);

    // Initialization 
    config_init(&cfg);

    // Read the file. If there is an error, report it and exit. 
    if (!config_read_file(&cfg, scFileLocation))
    {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        exit(EX_NOINPUT);
    }

    // Read the parameter group.
    setting = config_lookup(&cfg, "params");
    if (setting != NULL)
    {
        // Read the integer
        if (config_setting_lookup_int(setting, "addAWGNBasebandTx", &tmpI))
        {
            sc->addAWGNBasebandTx=tmpI;
            if (verbose) printf("addAWGNBasebandTx: %d\n", tmpI);
        }

        // Read the integer
        if (config_setting_lookup_int(setting, "addAWGNBasebandRx", &tmpI))
        {
            sc->addAWGNBasebandRx=tmpI;
            if (verbose) printf("addAWGNBasebandRx: %d\n", tmpI);
        }
        // Read the double
        if (config_setting_lookup_float(setting, "noiseSNR", &tmpD))
        {
            sc->noiseSNR=(float) tmpD;
            if (verbose) printf("Noise SNR: %f\n", tmpD);
        }
       
        // Read the double
        if (config_setting_lookup_float(setting, "noiseDPhi", &tmpD))
        {
            sc->noiseDPhi=(float) tmpD;
            if (verbose) printf("NoiseDPhi: %f\n", tmpD);
        }

        // Read the integer
        if (config_setting_lookup_int(setting, "addRicianFadingBasebandTx", &tmpI))
        {
            sc->addRicianFadingBasebandTx=tmpI;
            if (verbose) printf("addRicianFadingBasebandTx: %d\n", tmpI);
        }
        if (config_setting_lookup_int(setting, "addRicianFadingBasebandRx", &tmpI))
        {
            sc->addRicianFadingBasebandRx=tmpI;
            if (verbose) printf("addRicianFadingBasebandRx: %d\n", tmpI);
        }

		// Read the double
        if (config_setting_lookup_float(setting, "fadeK", &tmpD))
        {
            sc->fadeK=(float)tmpD;
            if (verbose) printf("fadeK: %f\n", tmpD);
        }
       
        // Read the double
        if (config_setting_lookup_float(setting, "fadeFd", &tmpD))
        {
            sc->fadeFd=(float)tmpD;
            if (verbose) printf("fadeFd: %f\n", tmpD);
        }
        
        // Read the double
        if (config_setting_lookup_float(setting, "fadeDPhi", &tmpD))
        {
            sc->fadeDPhi=(float)tmpD;
            if (verbose) printf("fadeDPhi: %f\n", tmpD);
        }

		// Read the integer
		if (config_setting_lookup_int(setting, "addCWInterfererBasebandTx", &tmpI))
        {
            sc->addCWInterfererBasebandTx=(float)tmpI;
            if (verbose) printf("addCWIntefererBasebandTx: %d\n", tmpI);
        }
		// Read the integer
		if (config_setting_lookup_int(setting, "addCWInterfererBasebandRx", &tmpI))
        {
            sc->addCWInterfererBasebandRx=(float)tmpI;
            if (verbose) printf("addCWIntefererBasebandRx: %d\n", tmpI);
        }

		// Read the double
		if (config_setting_lookup_float(setting, "cw_pow", &tmpD))
        {
            sc->cw_pow=(float)tmpD;
            if (verbose) printf("cw_pow: %f\n", tmpD);
        }

		// Read the double
		if (config_setting_lookup_float(setting, "cw_freq", &tmpD))
        {
            sc->cw_freq=(float)tmpD;
            if (verbose) printf("cw_freq: %f\n", tmpD);
        }
    }

    config_destroy(&cfg);

    return 1;
} // End readScConfigFile()

// Add AWGN
void enactAWGNBaseband(std::complex<float> * transmit_buffer, unsigned int buffer_len, struct CognitiveEngine *ce_ptr, struct Scenario *sc_ptr)
{
    //options
    float dphi  = sc_ptr->noiseDPhi;                              // carrier frequency offset
    float SNRdB = sc_ptr->noiseSNR;                               // signal-to-noise ratio [dB]
    // noise parameters
    float nstd = powf(10.0f, -SNRdB/20.0f); // noise standard deviation
    float phi = 0.0f;                       // channel phase
   
    std::complex<float> tmp (0, 1); 
    unsigned int i;

    // noise mixing
    for (i=0; i<buffer_len; i++) {
        transmit_buffer[i] = std::exp(tmp*phi) * transmit_buffer[i]; // apply carrier offset
        phi += dphi;                                 // update carrier phase
        cawgn(&transmit_buffer[i], nstd);            // add noise
    }
} // End enactAWGNBaseband()

void enactCWInterfererBaseband(std::complex<float> * transmit_buffer, unsigned int buffer_len, struct CognitiveEngine *ce_ptr, struct Scenario *sc_ptr)
{
	float fs = ce_ptr->bandwidth; // Sample rate of the transmit buffer
	float k = pow(10.0, sc_ptr->cw_pow/20.0); // Coefficient to set the interferer power correctly
	for(unsigned int i=0; i<buffer_len; i++)
	{
		transmit_buffer[i] += k*sin(6.283*sc_ptr->cw_freq*i/fs); // Add CW tone
	}
} // End enactCWInterfererBaseband()

// Add Rice-K Fading
void enactRicianFadingBaseband(std::complex<float> * transmit_buffer, unsigned int buffer_len, struct CognitiveEngine *ce_ptr, struct Scenario *sc_ptr)
{
    unsigned int h_len;                                     // doppler filter length
    if (buffer_len > 94){
        h_len = 0.0425*buffer_len;
    }
    else {
        h_len = 4;
    }
    float fd           = sc_ptr->fadeFd; // maximum doppler frequency
    float K            = sc_ptr->fadeK;  // Rice fading factor
    float omega        = 1.0f;      // mean power
    float theta        = 0.0f;      // angle of arrival
    float dphi 		   = sc_ptr->fadeDPhi;       // carrier frequency offset
    float phi          = 0.0f;               // channel phase

    // validate input
    if (K < 1.5f) {
        fprintf(stderr, "error: fading factor K must be greater than 1.5\n");
        exit(1);
    } else if (omega < 0.0f) {
        fprintf(stderr, "error: signal power Omega must be greater than zero\n");
        exit(1);
    } else if (fd <= 0.0f || fd >= 0.5f) {
        fprintf(stderr, "error: Doppler frequency must be in (0,0.5)\n");
        exit(1);
    //} else if (symbol_len== 0) {
    } else if (buffer_len== 0) {
        fprintf(stderr, "error: number of samples must be greater than zero\n");
        exit(1);
    }
 
    unsigned int i;

    // allocate array for output samples
    std::complex<float> * y = (std::complex<float> *) malloc(buffer_len*sizeof(std::complex<float>));
    // generate Doppler filter coefficients
    float h[h_len];
    liquid_firdes_doppler(h_len, fd, K, theta, h);

    // normalize filter coefficients such that output Gauss random
    // variables have unity variance
    float std = 0.0f;
    for (i=0; i<h_len; i++)
        std += h[i]*h[i];
    std = sqrtf(std);
    for (i=0; i<h_len; i++)
        h[i] /= std;

    // create Doppler filter from coefficients
    firfilt_crcf fdoppler = firfilt_crcf_create(h,h_len);

    // generate complex circular Gauss random variables
    std::complex<float> v;    // circular Gauss random variable (uncorrelated)
    std::complex<float> x;    // circular Gauss random variable (correlated w/ Doppler filter)
    float s   = sqrtf((omega*K)/(K+1.0));
    float sig = sqrtf(0.5f*omega/(K+1.0));
        
    std::complex<float> tmp(0, 1);
    for (i=0; i<buffer_len; i++) {
        // generate complex Gauss random variable
        crandnf(&v);

        // push through Doppler filter
        firfilt_crcf_push(fdoppler, v);
        firfilt_crcf_execute(fdoppler, &x);

        // convert result to random variable with Rice-K distribution
        y[i] = tmp*( std::imag(x)*sig + s ) +
                          ( std::real(x)*sig     );
    }
    for (i=0; i<buffer_len; i++) {
        transmit_buffer[i] *= std::exp(tmp*phi);  // apply carrier offset
        phi += dphi;                                  // update carrier phase
        transmit_buffer[i] *= y[i];                   // apply Rice-K distribution
    }

    // destroy filter object
    firfilt_crcf_destroy(fdoppler);

    // clean up allocated array
    free(y);
} // End enactRicianFadingBaseband()

//TODO: enable starting of this thread when a new scenario begins
// This function runs in its own thread waiting to modify the samples every time
// they are recieve by the ofdmtxrx object, but before they are sent to the
// synchronizer. 
void * enactScenarioBasebandRx( void * _arg)
{
    enactScenarioBasebandRxStruct * esbrs = (enactScenarioBasebandRxStruct *) _arg;
    int count = 0;
    pthread_mutex_lock(&esbrs->txcvr_ptr->rx_buffer_mutex);
    pthread_cond_signal(&(esbrs->txcvr_ptr->esbrs_ready));
    while (true)
    { 	
    	// Wait for txcvr rx_worker to signal samples are ready to be modified
		count++;
		pthread_cond_wait(&esbrs->txcvr_ptr->rx_buffer_filled_cond, &esbrs->txcvr_ptr->rx_buffer_mutex);

        // Add appropriate RF impairments for the scenario
        if (esbrs->sc_ptr->addRicianFadingBasebandRx == 1)
        {
            enactRicianFadingBaseband(esbrs->txcvr_ptr->rx_buffer->data(), esbrs->txcvr_ptr->rx_buffer->size(), esbrs->ce_ptr, esbrs->sc_ptr);
        }
        if (esbrs->sc_ptr->addCWInterfererBasebandRx == 1)
        {
            enactCWInterfererBaseband(esbrs->txcvr_ptr->rx_buffer->data(), esbrs->txcvr_ptr->rx_buffer->size(), esbrs->ce_ptr, esbrs->sc_ptr);
        }
        if (esbrs->sc_ptr->addAWGNBasebandRx == 1)
        {
            enactAWGNBaseband(esbrs->txcvr_ptr->rx_buffer->data(), esbrs->txcvr_ptr->rx_buffer->size(), esbrs->ce_ptr, esbrs->sc_ptr);
        }
	
        // signal to txcvr rx_worker that samples are ready to be sent to synchronizer
		pthread_cond_signal(&(esbrs->txcvr_ptr->rx_buffer_modified_cond));

        //TODO implement killing of this thread when a scenario ends.
    }
    return NULL;
} // End enactScenarioBasebandRx()

// Enact Scenario
void enactScenarioBasebandTx(std::complex<float> * transmit_buffer, unsigned int buffer_len, struct CognitiveEngine *ce_ptr, struct Scenario *sc_ptr)
{
    // Add appropriate RF impairments for the scenario
    if (sc_ptr->addRicianFadingBasebandTx == 1)
    {
        enactRicianFadingBaseband(transmit_buffer, buffer_len, ce_ptr, sc_ptr);
    }
    if (sc_ptr->addCWInterfererBasebandTx == 1)
    {
        enactCWInterfererBaseband(transmit_buffer, buffer_len, ce_ptr, sc_ptr);
    }
    if (sc_ptr->addAWGNBasebandTx == 1)
    {
        enactAWGNBaseband(transmit_buffer, buffer_len, ce_ptr, sc_ptr);
    }
    if ( (sc_ptr->addAWGNBasebandTx == 0) && (sc_ptr->addCWInterfererBasebandTx == 0) && (sc_ptr->addRicianFadingBasebandTx == 0))
    {
       	//fprintf(stderr, "WARNING: Nothing Added by Scenario!\n");
    }
} // End enactScenarioBasebandTx()

void * call_uhd_siggen(void * param)
{

    return NULL;
} // end call_uhd_siggen()

modulation_scheme convertModScheme(char * modScheme, unsigned int * bps)
{
    modulation_scheme ms;
    // TODO: add other liquid-supported mod schemes
    if (strcmp(modScheme, "QPSK") == 0) {
        ms = LIQUID_MODEM_QPSK;
		*bps = 2;
    }
    else if ( strcmp(modScheme, "BPSK") ==0) {
        ms = LIQUID_MODEM_BPSK;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "OOK") ==0) {
        ms = LIQUID_MODEM_OOK;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "8PSK") ==0) {
        ms = LIQUID_MODEM_PSK8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16PSK") ==0) {
        ms = LIQUID_MODEM_PSK16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32PSK") ==0) {
        ms = LIQUID_MODEM_PSK32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64PSK") ==0) {
        ms = LIQUID_MODEM_PSK64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "128PSK") ==0) {
        ms = LIQUID_MODEM_PSK128;
		*bps = 7;
    }
    else if ( strcmp(modScheme, "8QAM") ==0) {
        ms = LIQUID_MODEM_QAM8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16QAM") ==0) {
        ms = LIQUID_MODEM_QAM16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32QAM") ==0) {
        ms = LIQUID_MODEM_QAM32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64QAM") ==0) {
        ms = LIQUID_MODEM_QAM64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "BASK") ==0) {
        ms = LIQUID_MODEM_ASK2;
		*bps = 1;
    }
    else if ( strcmp(modScheme, "4ASK") ==0) {
        ms = LIQUID_MODEM_ASK4;
		*bps = 2;
    }
    else if ( strcmp(modScheme, "8ASK") ==0) {
        ms = LIQUID_MODEM_ASK8;
		*bps = 3;
    }
    else if ( strcmp(modScheme, "16ASK") ==0) {
        ms = LIQUID_MODEM_ASK16;
		*bps = 4;
    }
    else if ( strcmp(modScheme, "32ASK") ==0) {
        ms = LIQUID_MODEM_ASK32;
		*bps = 5;
    }
    else if ( strcmp(modScheme, "64ASK") ==0) {
        ms = LIQUID_MODEM_ASK64;
		*bps = 6;
    }
    else if ( strcmp(modScheme, "128ASK") ==0) {
        ms = LIQUID_MODEM_ASK128;
		*bps = 7;
    }
    else {
        fprintf(stderr, "ERROR: Unknown Modulation Scheme");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }

    return ms;
} // End convertModScheme()

crc_scheme convertCRCScheme(char * crcScheme, int verbose)
{
    crc_scheme check;
    if (strcmp(crcScheme, "none") == 0) {
        check = LIQUID_CRC_NONE;
        if (verbose) printf("check = LIQUID_CRC_NONE\n");
    }
    else if (strcmp(crcScheme, "checksum") == 0) {
        check = LIQUID_CRC_CHECKSUM;
        if (verbose) printf("check = LIQUID_CRC_CHECKSUM\n");
    }
    else if (strcmp(crcScheme, "8") == 0) {
        check = LIQUID_CRC_8;
        if (verbose) printf("check = LIQUID_CRC_8\n");
    }
    else if (strcmp(crcScheme, "16") == 0) {
        check = LIQUID_CRC_16;
        if (verbose) printf("check = LIQUID_CRC_16\n");
    }
    else if (strcmp(crcScheme, "24") == 0) {
        check = LIQUID_CRC_24;
        if (verbose) printf("check = LIQUID_CRC_24\n");
    }
    else if (strcmp(crcScheme, "32") == 0) {
        check = LIQUID_CRC_32;
        if (verbose) printf("check = LIQUID_CRC_32\n");
    }
    else {
        fprintf(stderr, "ERROR: unknown CRC\n");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }

    return check;
} // End convertCRCScheme()

fec_scheme convertFECScheme(char * FEC, int verbose)
{
    // TODO: add other liquid-supported FEC schemes
    fec_scheme fec;
    if (strcmp(FEC, "none") == 0) {
        fec = LIQUID_FEC_NONE;
        if (verbose) printf("fec = LIQUID_FEC_NONE\n");
    }
    else if (strcmp(FEC, "Hamming74") == 0) {
        fec = LIQUID_FEC_HAMMING74;
        if (verbose) printf("fec = LIQUID_FEC_HAMMING74\n");
    }
    else if (strcmp(FEC, "Hamming128") == 0) {
        fec = LIQUID_FEC_HAMMING128;
        if (verbose) printf("fec = LIQUID_FEC_HAMMING128\n");
    }
    else if (strcmp(FEC, "Golay2412") == 0) {
        fec = LIQUID_FEC_GOLAY2412;
        if (verbose) printf("fec = LIQUID_FEC_GOLAY2412\n");
    }
    else if (strcmp(FEC, "SEC-DED2216") == 0) {
        fec = LIQUID_FEC_SECDED2216;
        if (verbose) printf("fec = LIQUID_FEC_SECDED2216\n");
    }
    else if (strcmp(FEC, "SEC-DED3932") == 0) {
        fec = LIQUID_FEC_SECDED3932;
        if (verbose) printf("fec = LIQUID_FEC_SECDED3932\n");
    }
    else if (strcmp(FEC, "SEC-DED7264") == 0) {
        fec = LIQUID_FEC_SECDED7264;
        if (verbose) printf("fec = LIQUID_FEC_SECDED7264\n");
    }
    else {
        fprintf(stderr, "ERROR: unknown FEC\n");
        exit(EXIT_FAILURE);
        //TODO: Rather than halt execution,
        // Skip current test if given an unknown parameter.
    }
    return fec;
} // End convertFECScheme()

// Create Frame generator with CE and Scenario parameters
ofdmflexframegen CreateFG(struct CognitiveEngine ce, struct Scenario sc, int verbose) {

    // Set Modulation Scheme
    if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
    modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

    // Set Cyclic Redundency Check Scheme
    crc_scheme check = convertCRCScheme(ce.crcScheme, verbose);

    // Set inner forward error correction scheme
    if (verbose) printf("Inner FEC: ");
    fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

    // Set outer forward error correction scheme
    // TODO: add other liquid-supported FEC schemes
    if (verbose) printf("Outer FEC: ");
    fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);

    // Frame generation parameters
    ofdmflexframegenprops_s fgprops;

    // Initialize Frame generator and Frame Synchronizer Objects
    ofdmflexframegenprops_init_default(&fgprops);
    fgprops.mod_scheme      = ms;
    fgprops.check           = check;
    fgprops.fec0            = fec0;
    fgprops.fec1            = fec1;

    ofdmflexframegen fg = ofdmflexframegen_create(ce.numSubcarriers, ce.CPLen, ce.taperLen, NULL, &fgprops);

    return fg;
} // End CreateFG()

int rxCallback(unsigned char *  _header,
               int              _header_valid,
               unsigned char *  _payload,
               unsigned int     _payload_len,
               int              _payload_valid,
               framesyncstats_s _stats,
               void *           _userdata)
{   
	struct rxCBstruct * rxCBS_ptr = (struct rxCBstruct *) _userdata;
    int verbose = rxCBS_ptr->verbose;
	msequence rx_ms = *rxCBS_ptr->rx_ms_ptr;

    // Variables for checking number of errors 
    int j;
    unsigned int m;
	unsigned int tx_byte;
	
	if(rxCBS_ptr->isController && rxCBS_ptr->usingUSRPs){
		// Read FB from the payload received OTA and write it to the FB struct
        if (_payload_valid)
        {
            pthread_mutex_lock(&rxCBS_ptr->fb_ptr->fb_mutex);
            struct feedbackStruct * _fbReceived = (struct feedbackStruct *) _payload;
            rxCBS_ptr->fb_ptr->header_valid = _fbReceived->header_valid;
            rxCBS_ptr->fb_ptr->payload_valid = _fbReceived->payload_valid;
            rxCBS_ptr->fb_ptr->payload_len = _fbReceived->payload_len;
            rxCBS_ptr->fb_ptr->payloadByteErrors = _fbReceived->payloadByteErrors;
			rxCBS_ptr->fb_ptr->payloadBitErrors = _fbReceived->payloadBitErrors;
            rxCBS_ptr->fb_ptr->evm = _fbReceived->evm;
            rxCBS_ptr->fb_ptr->rssi = _fbReceived->rssi;
            rxCBS_ptr->fb_ptr->cfo = _fbReceived->cfo;
            rxCBS_ptr->fb_ptr->iteration = _fbReceived->iteration;
            int sigrt = pthread_cond_signal(&rxCBS_ptr->fb_ptr->fb_cond);
            pthread_mutex_unlock(&rxCBS_ptr->fb_ptr->fb_mutex);
        }
        else
        {
            pthread_mutex_lock(&rxCBS_ptr->fb_ptr->fb_mutex);
            int sigrt = pthread_cond_signal(&rxCBS_ptr->fb_ptr->fb_cond);
            pthread_mutex_unlock(&rxCBS_ptr->fb_ptr->fb_mutex);
        }
	}
	else{
		
		// Read the feedback directly from the callback parameters
		struct feedbackStruct fb = {};
		fb.header_valid         =   _header_valid;
		fb.payload_valid        =   _payload_valid;
		fb.payload_len          =   _payload_len;
		fb.payloadByteErrors    =   0;
		fb.payloadBitErrors     =   0;
		fb.evm                  =   _stats.evm;
		fb.rssi                 =   _stats.rssi;
		fb.cfo                  =   _stats.cfo;	
		fb.iteration			=	0;
		fb.block_flag			=	0;

		for(int i=0; i<4; i++)	fb.iteration += _header[i+2]<<(8*(3-i));

		if (verbose)
		{
			printf("In rxCallback():\n");
			printf("Header: %i %i %i %i %i %i %i %i\n", _header[0], _header[1], 
			    _header[2], _header[3], _header[4], _header[5], _header[6], _header[7]);
			feedbackStruct_print(&fb);
		}

		// Calculate byte error rate and bit error rate for payload
		for (m=0; m<_payload_len; m++)
		{
			tx_byte = msequence_generate_symbol(rx_ms,8);
		    if (((int)_payload[m] != tx_byte))
		    {
		        fb.payloadByteErrors++;
		        for (j=0; j<8; j++)
		        {
					if ((_payload[m]&(1<<j)) != (tx_byte&(1<<j)))
		               fb.payloadBitErrors++;
		        }      
		    }           
		}             
        	
		// Data that will be sent to server
		// TODO: Send other useful data through feedback array
		if(rxCBS_ptr->isController){
            pthread_mutex_lock(&rxCBS_ptr->fb_ptr->fb_mutex);
			*rxCBS_ptr->fb_ptr = fb;
            pthread_cond_signal(&rxCBS_ptr->fb_ptr->fb_cond);
            pthread_mutex_unlock(&rxCBS_ptr->fb_ptr->fb_mutex);
		}
		else{
			// Receiver sends feedback over TCP link
			write(rxCBS_ptr->client, (void*)&fb, sizeof(fb));

			// Receiver sends feedback OTA
			int i = 0;
			unsigned char header[8] = {0};            // Must always be 8 bytes for ofdmflexframe
			unsigned char *fb_c_ptr = (unsigned char*)&fb;
			unsigned char payload[1000];
			// Generate data
			if (verbose) printf("\n\nGenerating data that will go in frame...\n");
			for (size_t k=0; k<sizeof(fb); k++){
				payload[k] = *fb_c_ptr;
				fb_c_ptr++;
			}
			
			for (i=sizeof(fb); i<(signed int)rxCBS_ptr->ce_ptr->payloadLen; i++)
				payload[i] = i;
			
			// Include frame number in header information
			if (verbose) printf("Frame Num: %u\n", rxCBS_ptr->ce_ptr->frameNumber);

			// Set Modulation Scheme
			char mod[30] = "BPSK";
			char FEC0[30] = "Hamming74";
			char FEC1[30] = "none";
			modulation_scheme ms = convertModScheme(mod, &rxCBS_ptr->ce_ptr->bitsPerSym);
			printf("Setting transceiver parameters\n");
            rxCBS_ptr->txrx_ptr->set_tx_gain_uhd(25.0);
            rxCBS_ptr->txrx_ptr->set_tx_gain_soft(-8.0);
			printf("Set transceiver parameters\n");
			// Set Cyclic Redundency Check Scheme
			//crc_scheme check = convertCRCScheme(ce.crcScheme);

			// Set inner forward error correction scheme
			if (verbose) printf("Inner FEC: ");
			fec_scheme fec0 = convertFECScheme(FEC0, verbose);

			// Set outer forward error correction scheme
			if (verbose) printf("Outer FEC: ");
			fec_scheme fec1 = convertFECScheme(FEC1, verbose);

			// Replace with txcvr methods that allow access to samples:
			rxCBS_ptr->txrx_ptr->assemble_frame(header, payload, rxCBS_ptr->ce_ptr->payloadLen, ms, fec0, fec1);
			printf("Assembled\n");
			int isLastSymbol = 0;
			while(!isLastSymbol)
			{
				isLastSymbol = rxCBS_ptr->txrx_ptr->write_symbol();
				rxCBS_ptr->txrx_ptr->transmit_symbol();
			}
			rxCBS_ptr->txrx_ptr->end_transmit_frame();
		}
	}// End else (not the controller)
    return 0;

} // end rxCallback()

ofdmflexframesync CreateFS(struct CognitiveEngine ce, struct Scenario sc, struct rxCBstruct* rxCBs_ptr)
{
     ofdmflexframesync fs =
	     ofdmflexframesync_create(ce.numSubcarriers, ce.CPLen, ce.taperLen, NULL, rxCallback, (void *) rxCBs_ptr);

     return fs;
} // End CreateFS();

void * serveTCPclient(void * _sc_ptr){
	struct serveClientStruct * sc_ptr = (struct serveClientStruct*) _sc_ptr;
	struct feedbackStruct read_buffer;
	int rflag;
	// Write feedback to file

	while(1){
		rflag = recv(sc_ptr->client, &read_buffer, sizeof(struct feedbackStruct), 0);
		if(rflag == 0 || rflag == -1){
			close(sc_ptr->client);
			printf("Socket failure\n");
			exit(1);
		}
	}
    return NULL;
}

// Create a TCP socket for the server and bind it to a port
// Then sit and listen/accept all connections and write the data
// to an array that is accessible to the CE
void * startTCPServer(void * _ss_ptr)
{
    struct serverThreadStruct * ss_ptr = (struct serverThreadStruct*) _ss_ptr;

    //  Local (server) address
    struct sockaddr_in servAddr;   
    // Parameters of client connection
    struct sockaddr_in clientAddr;              // Client address 
    socklen_t client_addr_size;  // Client address size
    int socket_to_client = -1;
    int reusePortOption = 1;

	pthread_t TCPServeClientThread[5]; // Threads for clients
	int client = 0; // Client counter
        
    // Create socket for incoming connections 
    int sock_listen;
    if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        fprintf(stderr, "Transmitter Failed to Create Server Socket.\n");
        exit(EXIT_FAILURE);
    }

    // Allow reuse of a port. See http://stackoverflow.com/questions/14388706/socket-options-so-reuseaddr-and-so-reuseport-how-do-they-differ-do-they-mean-t
    if (setsockopt(sock_listen, SOL_SOCKET, SO_REUSEPORT, (void*) &reusePortOption, sizeof(reusePortOption)) < 0 )
    {
        fprintf(stderr, " setsockopt() failed\n");
        exit(EXIT_FAILURE);
    }

    // Construct local (server) address structure 
    memset(&servAddr, 0, sizeof(servAddr));       // Zero out structure 
    servAddr.sin_family = AF_INET;                // Internet address family 
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface 
    servAddr.sin_port = htons(ss_ptr->serverPort);              // Local port 
    // Bind to the local address to a port
    if (bind(sock_listen, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0)
    {
        fprintf(stderr, "ERROR: bind() error\n");
        exit(EXIT_FAILURE);
    }

    // Listen and accept connections indefinitely
    while (1)
    {
        // listen for connections on socket
        if (listen(sock_listen, MAXPENDING) < 0)
        {
            fprintf(stderr, "ERROR: Failed to Set Sleeping (listening) Mode\n");
            exit(EXIT_FAILURE);
        }

        // Accept a connection from client
        socket_to_client = accept(sock_listen, (struct sockaddr *)&clientAddr, &client_addr_size);
        if(socket_to_client< 0)
        {
            fprintf(stderr, "ERROR: Sever Failed to Connect to Client\n");
            exit(EXIT_FAILURE);
        }
		// Create separate thread for each client as they are accepted.
		else {
			struct serveClientStruct sc = CreateServeClientStruct();
			sc.client = socket_to_client;
			sc.fb_ptr = ss_ptr->fb_ptr;
			sc.OTA = ss_ptr->OTA;
			sc.usingUSRPs = ss_ptr->usingUSRPs;
			pthread_create( &TCPServeClientThread[client], NULL, serveTCPclient, (void*) &sc);
			client++;
			*ss_ptr->client_ptr = socket_to_client;
		}
	}// End While loop	
} // End startTCPServer()

int ceProcessData(struct CognitiveEngine * ce, struct feedbackStruct * fbPtr, int verbose)
{
    if (verbose)
    {
        printf("In ceProcessData():\n");
        feedbackStruct_print(fbPtr);
    }

    ce->validPayloads += fbPtr->payload_valid;

    if (fbPtr->payload_valid && (!(fbPtr->payloadBitErrors)))
    {
        ce->errorFreePayloads++;
		ce->errorFreePayloads_RA_ptr->update(1.0);
        if (verbose) printf("Error Free payload!\n");
    }

    ce->PER = ((float)ce->frameNumber-(float)ce->errorFreePayloads)/((float)ce->frameNumber);
    ce->lastReceivedFrame = fbPtr->iteration;
    ce->BER = ((float)fbPtr->payloadBitErrors)/((float)(ce->payloadLen*8));
    ce->weightedAvg += (float) fbPtr->payload_valid;

	ce->BER_RA_ptr->update(ce->BER);
	ce->PER_RA_ptr->update(ce->PER);
	ce->validPayloads_RA_ptr->update((float)fbPtr->payload_valid);

    // Update goal value
    if (strcmp(ce->goal, "payload_valid") == 0)
    {
        if (verbose) printf("Goal is payload_valid. Setting latestGoalValue to %d\n", fbPtr->payload_valid);
        ce->latestGoalValue = fbPtr->payload_valid;
    }
    else if (strcmp(ce->goal, "X_valid_payloads") == 0)
    {
        if (verbose) printf("Goal is X_valid_payloads. Setting latestGoalValue to %u\n", ce->validPayloads);
        ce->latestGoalValue = (float) ce->validPayloads;
    }
    else if (strcmp(ce->goal, "X_errorFreePayloads") == 0)
    {
        if (verbose) printf("Goal is X_errorFreePayloads. Setting latestGoalValue to %u\n", ce->errorFreePayloads);
        ce->latestGoalValue  = (float) ce->errorFreePayloads;
    }
    else if (strcmp(ce->goal, "X_frames") == 0)
    {
        if (verbose) printf("Goal is X_frames. Setting latestGoalValue to %u\n", ce->frameNumber);
        ce->latestGoalValue  = (float) ce->frameNumber;
    }
    else if (strcmp(ce->goal, "X_seconds") == 0)
    {       
		if (verbose) printf("Goal is X_seconds. Setting latestGoalValue to %f\n", ce->runningTime);
        ce->latestGoalValue = ce->runningTime;
    }
    else
    {
        fprintf(stderr, "ERROR: Unknown Goal!\n");
        exit(EXIT_FAILURE);
    }
    // TODO: implement if statements for other possible goals

    return 1;
} // End ceProcessData()

int ceOptimized(struct CognitiveEngine * ce, int verbose)
{
	// Update running average
	ce->averagedGoalValue -= ce->goal_mem[ce->iteration%ce->goal_averaging]/ce->goal_averaging;
	ce->averagedGoalValue += ce->latestGoalValue/ce->goal_averaging;
	ce->goal_mem[ce->iteration%ce->goal_averaging] = ce->latestGoalValue;
	
   	if(ce->frameNumber>ce->goal_averaging){
		if (verbose) 
	   	{
		   printf("Checking if goal value has been reached.\n");
		   printf("ce.averagedGoalValue= %f\n", ce->averagedGoalValue);
		   printf("ce.threshold= %f\n", ce->threshold);
	   	}
	   	if (ce->latestGoalValue >= ce->threshold)
	   	{
		   if (verbose) printf("Goal is reached!\n");
		   return 1;
	   	}
	   	if (verbose) printf("Goal not reached yet.\n");
	}
   	return 0;
} // end ceOptimized()

int ceModifyTxParams(struct CognitiveEngine * ce, struct feedbackStruct * fbPtr, int verbose)
{
    int modify = 0;


    if (verbose) printf("ce->adaptationCondition= %s\n", ce->adaptationCondition);

    // Add 'user input' adaptation
    if(strcmp(ce->adaptationCondition, "user_specified") == 0) {
        // Check if parameters should be modified
        modify = 1;
        if (verbose) printf("user specified adaptation mode. Modifying...\n");
    }

    // Check what values determine if parameters should be modified
    if(strcmp(ce->adaptationCondition, "last_payload_invalid") == 0) {
        // Check if parameters should be modified
        if(fbPtr->payload_valid<1)
        {
            modify = 1;
            if (verbose) printf("lpi. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "weighted_avg_payload_valid<X") == 0) {
        // Check if parameters should be modified
        if (ce->weightedAvg < ce->weighted_avg_payload_valid_threshold)
        {
            modify = 1;
            if (verbose) printf("wapv<X. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "weighted_avg_payload_valid>X") == 0) {
        // Check if parameters should be modified
        if (ce->weightedAvg > ce->weighted_avg_payload_valid_threshold)
        {
            modify = 1;
            if (verbose) printf("wapv>X. Modifying...\n");
        }
    }
    if(strcmp(ce->adaptationCondition, "PER<X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("PER = %f\n", ce->PER);
        if(ce->PER < ce->PER_threshold)
        {
            modify = 1;
            if (verbose) printf("per<x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "PER>X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("PER = %f\n", ce->PER);
        if(ce->PER > ce->PER_threshold)
        {
            modify = 1;
            if (verbose) printf("per>x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "BER_lastPacket<X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("BER = %f\n", ce->BER);
        if(ce->BER < ce->BER_threshold)
        {
            modify = 1;
            if (verbose) printf("Ber_lastpacket<x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "BER_lastPacket>X") == 0) {
        // Check if parameters should be modified
        if (verbose) printf("BER = %f\n", ce->BER);
        if(ce->BER > ce->BER_threshold)
        {
            modify = 1;
            if (verbose) printf("Ber_lastpacket>x. Modifying...\n" );
        }
    }
    if(strcmp(ce->adaptationCondition, "last_packet_error_free") == 0) {
        // Check if parameters should be modified
        if(!(fbPtr->payloadBitErrors)){
            modify = 1;
            if (verbose) printf("lpef. Modifying...\n");
        }
    }

    // If so, modify the specified parameter
    if (modify) 
    {
        if (verbose) printf("Modifying Tx parameters...\n");
        // TODO: Implement a similar if statement for each possible option
        // that can be adapted.

        if(strcmp(ce->adaptationCondition, "user_specified") == 0) {
            // Check if parameters should be modified
            if (verbose) printf("Reading user specified adaptations from user ce file: 'userEngine.txt'\n");
            readCEConfigFile(ce, (char*) "userEngine.txt", verbose);
        }

        if (strcmp(ce->adaptation, "increase_payload_len") == 0) {
            if (ce->payloadLen + ce->payloadLenIncrement <= ce->payloadLenMax) 
            {
                ce->payloadLen += ce->payloadLenIncrement;
            }
        }

        if (strcmp(ce->adaptation, "decrease_payload_len") == 0) {
            if (ce->payloadLen - ce->payloadLenIncrement >= ce->payloadLenMin) 
            {
                ce->payloadLen -= ce->payloadLenIncrement;
            }
        }

        if (strcmp(ce->adaptation, "decrease_mod_scheme_PSK") == 0) {
            if (strcmp(ce->modScheme, "QPSK") == 0) {
                strcpy(ce->modScheme, "BPSK");
            }
            if (strcmp(ce->modScheme, "8PSK") == 0) {
                strcpy(ce->modScheme, "QPSK");
            }
            if (strcmp(ce->modScheme, "16PSK") == 0) {
                strcpy(ce->modScheme, "8PSK");
            }
            if (strcmp(ce->modScheme, "32PSK") == 0) {
                strcpy(ce->modScheme, "16PSK");
            }
            if (strcmp(ce->modScheme, "64PSK") == 0) {
                strcpy(ce->modScheme, "32PSK");
            }
            if (strcmp(ce->modScheme, "128PSK") == 0) {
                strcpy(ce->modScheme, "64PSK");
            }
        }
        // Decrease ASK Modulations
        if (strcmp(ce->adaptation, "decrease_mod_scheme_ASK") == 0) {
            if (strcmp(ce->modScheme, "4ASK") == 0) {
                strcpy(ce->modScheme, "BASK");
            }
            if (strcmp(ce->modScheme, "8ASK") == 0) {
                strcpy(ce->modScheme, "4ASK");
            }
            if (strcmp(ce->modScheme, "16ASK") == 0) {
                strcpy(ce->modScheme, "8ASK");
            }
            if (strcmp(ce->modScheme, "32ASK") == 0) {
                strcpy(ce->modScheme, "16ASK");
	    }
            if (strcmp(ce->modScheme, "64ASK") == 0) {
                strcpy(ce->modScheme, "32ASK");
            }
            if (strcmp(ce->modScheme, "128ASK") == 0) {
                strcpy(ce->modScheme, "64ASK");
            }
        }
	// Turn outer FEC on/off
   	if (strcmp(ce->adaptation, "Outer FEC On/Off") == 0){
        if (verbose) printf("Adapt option: outer fec on/off. adapting...\n");
   	    // Turn FEC off
   	    if (ce->FECswitch == 1) {
   	        strcpy(ce->outerFEC_prev, ce->outerFEC);
   	        strcpy(ce->outerFEC, "none");
   	        ce->FECswitch = 0;
   	    }
   	    // Turn FEC on
   	    else {
   	        strcpy(ce->outerFEC, ce->outerFEC_prev);
   	        ce->FECswitch = 1;
   	    }
   	} 
        // Not use FEC
        if (strcmp(ce->adaptation, "no_fec") == 0) {
           if (strcmp(ce->outerFEC, "none") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "none");
           }
        }
        // FEC modifying (change to higher)
        if (strcmp(ce->adaptation, "increase_fec") == 0) {
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "SEC-DED7264");
           } 
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "SEC-DED3932");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "SEC-DED2216");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "Golay2412");
           }
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "Hamming128");
           }
           if (strcmp(ce->outerFEC, "none") == 0) {
               strcpy(ce->outerFEC, "Hamming74");
           }
        }
        // FEC modifying (change to lower)
        if (strcmp(ce->adaptation, "decrease_fec") == 0) {
           if (strcmp(ce->outerFEC, "Hamming74") == 0) {
               strcpy(ce->outerFEC, "none");
           }
           if (strcmp(ce->outerFEC, "Hamming128") == 0) {
               strcpy(ce->outerFEC, "Hamming74");
           }
           if (strcmp(ce->outerFEC, "Golay2412") == 0) {
               strcpy(ce->outerFEC, "Hamming128");
           }
           if (strcmp(ce->outerFEC, "SEC-DED2216") == 0) {
               strcpy(ce->outerFEC, "Golay2412");
           }
           if (strcmp(ce->outerFEC, "SEC-DED3932") == 0) {
               strcpy(ce->outerFEC, "SEC-DED2216");
           }
           if (strcmp(ce->outerFEC, "SEC-DED7264") == 0) {
               strcpy(ce->outerFEC, "SEC-DED3932");
           }
        }

        if (strcmp(ce->adaptation, "mod_scheme->BPSK") == 0) {
            strcpy(ce->modScheme, "BPSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->QPSK") == 0) {
            strcpy(ce->modScheme, "QPSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8PSK") == 0) {
            strcpy(ce->modScheme, "8PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16PSK") == 0) {
            strcpy(ce->modScheme, "16PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->328PSK") == 0) {
            strcpy(ce->modScheme, "32PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64PSK") == 0) {
            strcpy(ce->modScheme, "64PSK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8QAM") == 0) {
            strcpy(ce->modScheme, "8QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16QAM") == 0) {
            strcpy(ce->modScheme, "16QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->32QAM") == 0) {
            strcpy(ce->modScheme, "32QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64QAM") == 0) {
            strcpy(ce->modScheme, "64QAM");
        }
        if (strcmp(ce->adaptation, "mod_scheme->OOK") == 0) {
            strcpy(ce->modScheme, "OOK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->4ASK") == 0) {
            strcpy(ce->modScheme, "4ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->8ASK") == 0) {
            strcpy(ce->modScheme, "8ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->16ASK") == 0) {
            strcpy(ce->modScheme, "16ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->32ASK") == 0) {
            strcpy(ce->modScheme, "32ASK");
        }
        if (strcmp(ce->adaptation, "mod_scheme->64ASK") == 0) {
            strcpy(ce->modScheme, "64ASK");
        }
    }
    return 1;
}   // End ceModifyTxParams()

int postTxTasks(struct CognitiveEngine * cePtr, struct feedbackStruct * fb_ptr, int verbose)
{
    // FIXME: Find another way to fix this:: FIXED?
    int DoneTransmitting = 0;

    // Process data from rx
    ceProcessData(cePtr, fb_ptr, verbose);
    // Modify transmission parameters (in fg and in USRP) accordingly
    if (!ceOptimized(cePtr, verbose)) 
    {
        if (verbose) printf("ceOptimized() returned false\n");
        ceModifyTxParams(cePtr, fb_ptr, verbose);
    }
    else
    {
        if (verbose) printf("ceOptimized() returned true\n");
        DoneTransmitting = 1;
    }

    // For debugging
    if (verbose)
    {
        printf("in postTxTasks(): \nce.numSubcarriers= %u\n", cePtr->numSubcarriers);
        printf("ce.CPLen= %u\n", cePtr->CPLen);
    }

    return DoneTransmitting;
} // End postTxTasks()

void updateScenarioSummary(struct scenarioSummaryInfo *sc_sum, struct feedbackStruct *fb, struct CognitiveEngine *ce, int i_CE, int i_Sc){
	sc_sum->valid_headers[i_CE][i_Sc] += fb->header_valid;
	sc_sum->valid_payloads[i_CE][i_Sc] += fb->payload_valid;
	sc_sum->EVM[i_CE][i_Sc] += fb->evm;
	sc_sum->RSSI[i_CE][i_Sc] += fb->rssi;
	sc_sum->total_bits[i_CE][i_Sc] += ce->payloadLen;
	sc_sum->bit_errors[i_CE][i_Sc] += fb->payloadBitErrors;
}

void updateCognitiveEngineSummaryInfo(struct cognitiveEngineSummaryInfo *ce_sum, struct scenarioSummaryInfo *sc_sum, struct CognitiveEngine *ce, int i_CE, int i_Sc){
	// Decrement frameNumber once
	ce->frameNumber--;
	// Store metrics for scenario
	sc_sum->total_frames[i_CE][i_Sc] = ce->frameNumber;
	sc_sum->EVM[i_CE][i_Sc] /= ce->frameNumber;
	sc_sum->RSSI[i_CE][i_Sc] /= ce->frameNumber;
	sc_sum->PER[i_CE][i_Sc] = ce->PER;

	// Display the scenario summary
	printf("Cognitive Engine %i Scenario %i Summary:\nTotal frames: %i\nPercent valid headers: %2f\nPercent valid payloads: %2f\nAverage EVM: %2f\n"
		"Average RSSI: %2f\nAverage BER: %2f\nAverage PER: %2f\n\n", i_CE+1, i_Sc+1, sc_sum->total_frames[i_CE][i_Sc],
		(float)sc_sum->valid_headers[i_CE][i_Sc]/(float)sc_sum->total_frames[i_CE][i_Sc], (float)sc_sum->valid_payloads[i_CE][i_Sc]/(float)sc_sum->total_frames[i_CE][i_Sc],
		sc_sum->EVM[i_CE][i_Sc], sc_sum->RSSI[i_CE][i_Sc], (float)sc_sum->bit_errors[i_CE][i_Sc]/(float)sc_sum->total_bits[i_CE][i_Sc], sc_sum->PER[i_CE][i_Sc]);

	// Store the sum of scenario metrics for the cognitive engine
	ce_sum->total_frames[i_CE] += sc_sum->total_frames[i_CE][i_Sc];
	ce_sum->valid_headers[i_CE] += sc_sum->valid_headers[i_CE][i_Sc];
	ce_sum->valid_payloads[i_CE] += sc_sum->valid_payloads[i_CE][i_Sc];
	ce_sum->EVM[i_CE] += sc_sum->EVM[i_CE][i_Sc];
	ce_sum->RSSI[i_CE] += sc_sum->RSSI[i_CE][i_Sc];
	ce_sum->total_bits[i_CE] += sc_sum->total_bits[i_CE][i_Sc];
	ce_sum->bit_errors[i_CE] += sc_sum->bit_errors[i_CE][i_Sc];
	ce_sum->PER[i_CE] += sc_sum->PER[i_CE][i_Sc];
}

void uhd_quiet(uhd::msg::type_t type, const std::string &msg){}

void terminate(int sig){
	exit(1);
}

int main(int argc, char ** argv)
{
    // Seed the PRNG
    srand(time(NULL));

    // TEMPORARY VARIABLE
    int usingUSRPs = 0;

    int verbose = 1;
    int verbose_explicit = 0;
    int dataToStdout = 0;    

    // For experiments with CR Networks.
    // Specifies whether this crts instance is managing the experiment.
    int isController = 0;

    unsigned int serverPort = 1402;
    char * serverAddr = (char*) "127.0.0.1";

    // Frame Synchronizer parameters
    unsigned int numSubcarriers = 64;
    unsigned int CPLen = 16;
    unsigned int taperLen = 4;
    float bandwidth = 1.0e6;
    float uhd_rxgain = 31.5;
	float frequency_tx;
	float frequency_rx;

    // Check Program options
    int d;
    while ((d = getopt(argc,argv,"uhqvdrsp:ca:f:b:G:M:C:T:")) != EOF) {
        switch (d) {
        case 'u':
        case 'h':   usage();                           		return 0;
        case 'q':   verbose = 0;                            break;
        case 'v':   verbose = 1; verbose_explicit = 1;      break;
        case 'd':   dataToStdout = 1; 
                    if (!verbose_explicit) verbose = 0;     break;
        case 'r':   usingUSRPs = 1;                         break;
        case 's':   usingUSRPs = 0;                         break;
        case 'p':   serverPort = atoi(optarg);              break;
        case 'c':   isController = 1;                       break;
        case 'a':   serverAddr = optarg;                    break;
        case 'f':   frequency_tx = atof(optarg);            break;
		case 'F':	frequency_rx = atof(optarg);			break;
        case 'b':   bandwidth = atof(optarg);               break;
        case 'G':   uhd_rxgain = atof(optarg);              break;
        case 'M':   numSubcarriers = atoi(optarg);          break;
        case 'C':   CPLen = atoi(optarg);                   break;
        case 'T':   taperLen = atoi(optarg);                break;
        //case 'p':   serverPort = atol(optarg);            break;
        //case 'f':   frequency = atof(optarg);           break;
        //case 'b':   bandwidth = atof(optarg);           break;
        //case 'G':   uhd_rxgain = atof(optarg);          break;
        //case 't':   num_seconds = atof(optarg);         break;
        default:
            verbose = 1;
        }   
    }

	// Default transmit and receive frequencies (reversed for controller/slaves)
	if(isController){
	    frequency_tx = 460.0e6;
		frequency_rx = 468.0e6;
	}
	else{
		frequency_tx = 468.0e6;
		frequency_rx = 460.0e6;
	}

    pthread_t TCPServerThread;   // Pointer to thread ID
    pthread_t enactScBbRxThread;   // Pointer to thread ID

    // Array that will be accessible to both Server and CE.
    // Server uses it to pass data to CE.
    struct feedbackStruct fb = {};

    // For creating appropriate symbol length from 
    // number of subcarriers and CP Length
    unsigned int symbolLen;

    // Iterators
    int i_CE = 0;
    int i_Sc = 0;
    int DoneTransmitting = 0;
    int isLastSymbol = 0;
    char scenario_list [30][60];
    char cogengine_list [30][60];

    int NumCE=readCEMasterFile(cogengine_list, verbose, isController);  
    int NumSc=readScMasterFile(scenario_list, verbose);  

    // Cognitive engine struct used in each test
    struct CognitiveEngine ce = CreateCognitiveEngine();
    // Scenario struct used in each test
    struct Scenario sc = CreateScenario();

    // framegenerator object used in each test
    ofdmflexframegen fg;

    // framesynchronizer object used in each test
    ofdmflexframesync fs;

	// Quiet UHD output if not verbose
	if(!verbose) uhd::msg::register_handler(&uhd_quiet);

	// Identical pseudo random sequence generators for tx and rx
	msequence tx_ms = msequence_create_default(9u);
	msequence rx_ms = msequence_create_default(9u);

    // Buffers for packet/frame data
    unsigned char header[8];                       // Must always be 8 bytes for ofdmflexframe
    unsigned char payload[1000];                   // Large enough to accomodate any (reasonable) payload that
                                                   // the CE wants to use.

    std::complex<float> frameSamples[10000];      // Buffer of frame samples for each symbol.
                                                   // Large enough to accomodate any (reasonable) payload that 
                                                   // the CE wants to use.
    // USRP objects
    uhd::tx_metadata_t metaData;
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::tx_streamer::sptr txStream;

	float throughput = 0;
	float total_symbols;
	float payload_symbols;

	// Metric Summary structs for each scenario and each cognitive engine
	struct scenarioSummaryInfo sc_sum;
	struct cognitiveEngineSummaryInfo ce_sum;
   
    // Statements for what information to print
	bool print_frame_info = true;
	bool print_validity_metrics = true;
	bool print_error_metrics = false;
	bool print_signal_quality_metrics = true;
	bool print_spectral_metrics = true;
	bool print_goal_metrics = true;
				 

    ////////////////////// End variable initializations.///////////////////////

	signal(SIGTERM, terminate);
	signal(SIGINT, terminate);
	signal(SIGQUIT, terminate);
	signal(SIGKILL, terminate);

    int client;
	
	// Begin TCP Server Thread for slave node(s) if using USRP's
	if(usingUSRPs && isController){
		struct serverThreadStruct ss_slave = CreateServerStruct();
		ss_slave.serverPort = serverPort;
		ss_slave.fb_ptr = &fb;
		ss_slave.client_ptr = &client;
		pthread_create( &TCPServerThread, NULL, startTCPServer, (void*) &ss_slave);
		printf("\nPress any key once all nodes have connected to the TCP server\n");
		getchar();
	}

    struct rxCBstruct rxCBs = CreaterxCBStruct();
    rxCBs.bandwidth = bandwidth;
    rxCBs.serverPort = serverPort;
    rxCBs.serverAddr = serverAddr;
    rxCBs.verbose = verbose;
	rxCBs.rx_ms_ptr = &rx_ms;
	rxCBs.isController = isController;
	rxCBs.usingUSRPs = usingUSRPs;
	rxCBs.fb_ptr = &fb;

    // Allow server time to finish initialization
    usleep(0.1e6);

	const int socket_to_server = socket(AF_INET, SOCK_STREAM, 0);
	if(!isController && usingUSRPs){
		// Create a client TCP socket] 
		if( socket_to_server < 0)
		{   
		    fprintf(stderr, "ERROR: Receiver Failed to Create Client Socket. \nerror: %s\n", strerror(errno));
		    exit(EXIT_FAILURE);
		}

		// Parameters for connecting to server
		struct sockaddr_in servAddr;
		memset(&servAddr, 0, sizeof(servAddr));
		servAddr.sin_family = AF_INET;
		servAddr.sin_port = htons(serverPort);
		servAddr.sin_addr.s_addr = inet_addr(serverAddr);

        if(verbose)
        {
            printf("Connecting to server at %s:%u\n", serverAddr, serverPort);
        }

		// Attempt to connect client socket to server
		int connect_status;
		if((connect_status = connect(socket_to_server, (struct sockaddr*)&servAddr, sizeof(servAddr))))
		{   
		    fprintf(stderr, "Failed to Connect to server.\n");
		    fprintf(stderr, "connect_status = %d\n", connect_status);
		    exit(EXIT_FAILURE);
		}

		rxCBs.client = socket_to_server;
        if (verbose) printf("Connected to Server.\n");
	}

    // Get current date and time
    char dataFilename[50];
    time_t now = time(NULL);
    struct tm *t  = localtime(&now);
    strftime(dataFilename, sizeof(dataFilename)-1, "data/%Y-%m-%d__%H-%M-%S__crts_data.txt", t);
    // TODO: Make sure data folder exists
    
    // Initialize Data File
    FILE * dataFile;
    if (isController)
    {
        if (dataToStdout)
        {
            dataFile = stdout;
        }
        else
        {
            dataFile = fopen(dataFilename, "w");
        }
    }

    // Begin running tests

    // For each Cognitive Engine
    for (i_CE=0; i_CE<NumCE; i_CE++)
    {

		if (verbose) 
            printf("\nStarting Tests on Cognitive Engine %d\n", i_CE+1);
            
        if(isController){
		    // Initialize current CE
			ce = CreateCognitiveEngine();
			readCEConfigFile(&ce,cogengine_list[i_CE], verbose);
			// Send CE info to slave node(s)
			if(usingUSRPs) write(client, (void*)&ce, sizeof(ce));
		}
        ce.frequency_tx = frequency_tx;
		ce.frequency_rx = frequency_rx;
        // Run each CE through each scenario
        for (i_Sc= 0; i_Sc<NumSc; i_Sc++)
        {                	
				
            if (isController)
            {                   
        		if (verbose) printf("\n\nStarting Scenario %d\n", i_Sc+1);
                // Initialize current Scenario
                sc = CreateScenario();
                readScConfigFile(&sc,scenario_list[i_Sc], verbose);
                
				// Send Sc info to slave node(s)
                if(usingUSRPs) write(client, (void*)&sc, sizeof(sc));	
				if (verbose) printf("\n\nStarting Scenario %d\n", i_Sc+1);
            	rxCBs.ce_ptr = &ce;
            	rxCBs.sc_ptr = &sc;

                fprintf(dataFile, "Cognitive Engine %d\nScenario %d\n", i_CE+1, i_Sc+1);
                
				/////////// Print metrics by category /////////////
				
				if(print_frame_info) fprintf(dataFile,"%-10s%-7s","Linetype","Frame");
				if(print_validity_metrics) fprintf(dataFile,"%-14s%-15s","Valid Header","Valid Payload");
				if(print_error_metrics) fprintf(dataFile,"%-13s%-12s%-7s%-7s%-9s%-9s","Byte Errors","Bit Errors","PER","BER","Avg PER","Avg BER");
				if(print_signal_quality_metrics) fprintf(dataFile,"%-10s%-11s","EVM (dB)","RSSI (dB)");
				if(print_spectral_metrics) fprintf(dataFile,"%-12s%-21s","Throughput", "Spectral Efficiency");
				if(print_goal_metrics) fprintf(dataFile,"%-16s","Avg Goal Value");
				fprintf(dataFile,"\n");
				if(print_frame_info) fprintf(dataFile,"----------------");
				if(print_validity_metrics) fprintf(dataFile,"-----------------------------");
				if(print_error_metrics) fprintf(dataFile,"---------------------------------------------------------");
				if(print_signal_quality_metrics) fprintf(dataFile,"---------------------");
				if(print_spectral_metrics) fprintf(dataFile,"--------------------------------");
				if(print_goal_metrics) fprintf(dataFile,"----------------");
				fprintf(dataFile,"\n");

				/////////////////////////////////////////////////////
				
				fflush(dataFile);
            }

            // Initialize Receiver Defaults for current CE and Sc
            ce.frameNumber = 1;
            fs = CreateFS(ce, sc, &rxCBs);

            std::clock_t begin = std::clock();
            std::clock_t now;
            // Begin Testing Scenario
            DoneTransmitting = 0;

            if (usingUSRPs) 
            { 
                // create transceiver object
                unsigned char * p = NULL;   // default subcarrier allocation
                if (verbose) 
                    printf("Using ofdmtxrx\n");                       

				bool rx_sim = false;
	    		if (!isController) rx_sim = true;
				ofdmtxrx *txcvr_ptr = new ofdmtxrx(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, rxCallback, (void*) &rxCBs, rx_sim);                                        
                    
				rxCBs.txrx_ptr = txcvr_ptr;
				// set properties
                txcvr_ptr->set_tx_freq(ce.frequency_tx);
                txcvr_ptr->set_tx_rate(ce.bandwidth);
                txcvr_ptr->set_tx_gain_soft(ce.txgain_dB);
                txcvr_ptr->set_tx_gain_uhd(ce.uhd_txgain_dB);
                //txcvr_ptr->set_tx_antenna("TX/RX");
				txcvr_ptr->set_rx_freq(ce.frequency_rx);
                txcvr_ptr->set_rx_rate(ce.bandwidth);
                txcvr_ptr->set_rx_gain_uhd(uhd_rxgain);	

				if (isController) txcvr_ptr->start_rx();

                // Each instance of this while loop transmits one packet
                while(!DoneTransmitting)
                {
					if (!isController)
                    {

                       	if (verbose)
                       	{
                           	txcvr_ptr->debug_enable();
                           	printf("Set Rx freq to %f\n", ce.frequency_rx);
                           	printf("Set Rx rate to %f\n", ce.bandwidth);
                           	printf("Set uhd Rx gain to %f\n", uhd_rxgain);
                       	}
                            
                       	// Structs for CE and Sc info
                       	struct CognitiveEngine ce_controller;
                       	struct Scenario sc_controller;
						
                       	int continue_running = 1;
						int rflag;
						char readbuffer[1000];

                       	// Receive CE info
                       	rflag = recv(socket_to_server, &readbuffer, sizeof(struct CognitiveEngine), 0);
                       	if(rflag == 0 || rflag == -1){
                       		printf("Error receiving CE info from the controller\n");
							close(socket_to_server);
							exit(1);
						}
		    			else ce_controller = *(struct CognitiveEngine*)readbuffer;
							
		    			// Receive Sc info
		    			rflag = recv(socket_to_server, &readbuffer, sizeof(struct Scenario), 0);
                        if(rflag == 0 || rflag == -1){
                           	printf("Error receiving Scenario info from the controller\n");
							close(socket_to_server);
							exit(1);
  			    		}
		    			else sc_controller = *(struct Scenario*)readbuffer;
							
		    			// Initialize members of esbrs struct sent to enactScenarioBasebandRx()
		    			//struct enactScenarioBasebandRxStruct esbrs = {.txcvr_ptr = txcvr_ptr, .ce_ptr = &ce_controller, .sc_ptr = &sc_controller};
		    			rxCBs.ce_ptr = &ce_controller;
						rxCBs.sc_ptr = &sc_controller;
						struct enactScenarioBasebandRxStruct esbrs = {
							.txcvr_ptr = txcvr_ptr, 
							.ce_ptr = &ce_controller, 
							.sc_ptr = &sc_controller
						};
		    			//pthread_mutex_init(&esbrs_ready_mutex, NULL);
						pthread_mutex_lock(&txcvr_ptr->rx_buffer_mutex);			
						pthread_create( &enactScBbRxThread, NULL, enactScenarioBasebandRx, (void*) &esbrs);
							
						// Wait until enactScenarioBasebandRx() has initialized
						pthread_cond_wait(&txcvr_ptr->esbrs_ready, &txcvr_ptr->rx_buffer_mutex);
						pthread_mutex_unlock(&txcvr_ptr->rx_buffer_mutex);			
						// Start liquid-usrp receiver
						printf("Starting receiver\n");
						txcvr_ptr->start_rx();
							
                        while(continue_running)
                        {
							// Wait until server provides more information, closes, or there is an error
							rflag = recv(socket_to_server, &readbuffer, sizeof(struct Scenario)+sizeof(struct CognitiveEngine), 0);
							if(rflag == 0 || rflag == -1){
								printf("Socket closed or failed\n");
				 				close(socket_to_server);
								msequence_destroy(rx_ms);
								exit(1);
							}
								
							//TODO:
                            // if new scenario:
                            //{
                                // close enactScenarioBasebandRx Thread
                                // close current ofdmtxrx object
                                // update sc and ce
                                // open new ofdmtxrx object
                                // open new enactScenarioBasebandRx Thread
                            //}
                            /*else if(rflag == sizeof(struct Scenario)){
                            	if(verbose) printf("Rewriting Scenario Info");
								pthread_cancel(enactScBbRxThread);
								delete txcvr_ptr;
								sc_controller = *(struct Scenario*)readbuffer;
								ofdmtxrx *txcvr_ptr = new ofdmtxrx(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, rxCallback, (void*) &rxCBs, true);
								struct enactScenarioBasebandRxStruct esbrs = {.txcvr_ptr = txcvr_ptr, .ce_ptr = &ce_controller, .sc_ptr = &sc_controller};							
								pthread_create( &enactScBbRxThread, NULL, enactScenarioBasebandRx, (void*) &esbrs);
							}
							else if(rflag == sizeof(struct CognitiveEngine)){
								if(verbose) printf("Rewriting CE info");
								pthread_cancel(enactScBbRxThread);
								delete txcvr_ptr;
								ce_controller = *(struct CognitiveEngine*)readbuffer;
								ofdmtxrx *txcvr_ptr = new ofdmtxrx(ce.numSubcarriers, ce.CPLen, ce.taperLen, p, rxCallback, (void*) &rxCBs, true);
								struct enactScenarioBasebandRxStruct esbrs = {.txcvr_ptr = txcvr_ptr, .ce_ptr = &ce_controller, .sc_ptr = &sc_controller};							
								pthread_create( &enactScBbRxThread, NULL, enactScenarioBasebandRx, (void*) &esbrs);
							}*/
                                
                       	}
                    }

                    if (verbose) {
                       	txcvr_ptr->debug_enable();
                       	printf("Set transmit frequency to %f\n", ce.frequency_tx);
                       	printf("Set bandwidth to %f\n", ce.bandwidth);
                       	printf("Set txgain_dB to %f\n", ce.txgain_dB);
                       	printf("Set uhd_txgain_dB to %f\n", ce.uhd_txgain_dB);
                       	printf("Set Tx antenna to %s\n", "TX/RX");
                    }

                    int i = 0;
                    // Generate data
                    if (verbose) printf("\n\nGenerating data that will go in frame...\n");
					header[0] = i_CE+1;
					header[1] = i_Sc+1;
                    for (i=0; i<4; i++)
                       	header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
					header[6] = 0;
					header[7] = 0;
                    for (i=0; i<(signed int)ce.payloadLen; i++)
                       	payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);

                    // Include frame number in header information
                    if (verbose) printf("Frame Num: %u\n", ce.frameNumber);

                    // Set Modulation Scheme
                    if (verbose) printf("Modulation scheme: %s\n", ce.modScheme);
                    modulation_scheme ms = convertModScheme(ce.modScheme, &ce.bitsPerSym);

                    // Set Cyclic Redundency Check Scheme
                    //crc_scheme check = convertCRCScheme(ce.crcScheme);

                    // Set inner forward error correction scheme
                    if (verbose) printf("Inner FEC: ");
                    fec_scheme fec0 = convertFECScheme(ce.innerFEC, verbose);

                    // Set outer forward error correction scheme
                    if (verbose) printf("Outer FEC: ");
                    fec_scheme fec1 = convertFECScheme(ce.outerFEC, verbose);

                    // Replace with txcvr methods that allow access to samples:
                    txcvr_ptr->assemble_frame(header, payload, ce.payloadLen, ms, fec0, fec1);
                    int isLastSymbol = 0;
                    while(!isLastSymbol)
                    {
                        isLastSymbol = txcvr_ptr->write_symbol();
						enactScenarioBasebandTx(txcvr_ptr->fgbuffer, txcvr_ptr->fgbuffer_len, &ce, &sc);
						txcvr_ptr->transmit_symbol();
                    }
                    txcvr_ptr->end_transmit_frame();
					//printf("Transmitted frame\n");
                        
					// Get current time. Then add delay to find out at what time to stop waiting. 
                    //TODO switch to boost library for more accuracy
   	                struct timeval timeNow;
       	            struct timespec releaseTime;
           	        gettimeofday(&timeNow, NULL);
               	    double wholeSecondsDelay = 0.0;
                   	//printf("delay = %f\n", ce.delay_us);
                    double delay_fpart_s = modf(ce.delay_us*1e-6, &wholeSecondsDelay);
   	                //printf("wholeSecondsDelay = %f\n", wholeSecondsDelay);

       	            //releaseTime.tv_sec = timeNow.tv_sec + (int) wholeSecondsDelay;
           	        //unsigned int wholeNsDelay = (int) delay_fpart_s*1e9;
               	    //releaseTime.tv_nsec = (timeNow.tv_usec*1000) + wholeNsDelay;

                    // Temporary Fix
       	            releaseTime.tv_sec = timeNow.tv_sec;
               	    releaseTime.tv_nsec = (timeNow.tv_usec*1000) + 20000000;

                   	// Lock the feedback struct mutex and
                    // Wait for either a signal, or until the delay time has passed.
   	                //printf("In main: locking fb_mutex\n");
       	            //pthread_mutex_lock(&fb.fb_mutex);
           	        if (verbose)
               	        printf("Frame transmitted. Waiting for feedback OTA or for timeout\n");
                   	int ptrt = pthread_cond_timedwait(&fb.fb_cond, &fb.fb_mutex, &releaseTime);
	
					DoneTransmitting = postTxTasks(&ce, &fb, verbose);
                    // Record the feedback data received
                    //TODO: include fb.cfo

					// Compute throughput and spectral efficiency
					payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
					total_symbols = (float)ofdmflexframegen_getframelen(txcvr_ptr->fg);
					throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

					/////////// Print metrics by category /////////////
				
					if(print_frame_info) fprintf(dataFile,"%-10s%-7i","crtsdata",ce.iteration);
					if(print_validity_metrics) fprintf(dataFile,"%-14i%-15i",fb.header_valid,fb.payload_valid);
					if(print_error_metrics) fprintf(dataFile,"%-13i%-12i%-7.2f%-7.2f%-9.2f%-9.2f",fb.payloadByteErrors,fb.payloadBitErrors,ce.PER,ce.BER,ce.PER_avg,ce.BER_avg);
					if(print_signal_quality_metrics) fprintf(dataFile,"%-10.2f%-11.2f",fb.evm,fb.rssi);
					if(print_spectral_metrics) fprintf(dataFile,"%-12.2f%-21.2f",throughput, throughput/ce.bandwidth);
					if(print_goal_metrics) fprintf(dataFile,"%-16.2f",ce.averagedGoalValue);
				    fprintf(dataFile,"\n");

					/////////////////////////////////////////////////////
					
					//All metrics
                    //fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-10.2f %-19u %-16.2f %-18u\n", //%-12.2f %-20.2f %-19.2f 
					//"crtsdata:", ce.iteration, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
					//ce.BER, fb.payloadBitErrors);//, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);
					//Useful metrics
					/*fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
					"crtsdata:", ce.iteration,  fb.evm, fb.rssi, ce.PER,
					ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/
                    fflush(dataFile);

					// Increment the frame counter
                    ce.frameNumber++;
					ce.iteration++;

                    // Update the clock
                    now = std::clock();
                    ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;

					updateScenarioSummary(&sc_sum, &fb, &ce, i_CE, i_Sc);
                } // End while not done transmitting loop
                // TODO: close ofdmtxrx object
            }
            else // If not using USRPs
            {
                while(!DoneTransmitting)
                {
                    // Initialize Transmitter Defaults for current CE and Sc
                    fg = CreateFG(ce, sc, verbose);  // Create ofdmflexframegen object with given parameters
                    if (verbose) ofdmflexframegen_print(fg);

                    // Iterator
                    int i = 0;

                    // Generate data
                    if (verbose) printf("\n\nGenerating data that will go in frame...\n");
                    header[0] = i_CE+1;
					header[1] = i_Sc+1;
                    for (i=0; i<4; i++)
                       	header[i+2] = (ce.frameNumber & (0xFF<<(8*(3-i))))>>(8*(3-i));
					header[6] = 0;
					header[7] = 0;
                    for (i=0; i<(signed int)ce.payloadLen; i++)
                        payload[i] = (unsigned char)msequence_generate_symbol(tx_ms,8);

					// Called just to update bits per symbol field
					convertModScheme(ce.modScheme, &ce.bitsPerSym);

                    // Assemble frame
                    ofdmflexframegen_assemble(fg, header, payload, ce.payloadLen);
                    //printf("DoneTransmitting= %d\n", DoneTransmitting);
						
                    // i.e. Need to transmit each symbol in frame.
                    isLastSymbol = 0;

                    while (!isLastSymbol) 
                    {
                        //isLastSymbol = txTransmitPacket(ce, &fg, frameSamples, metaData, txStream, usingUSRPs);
                        isLastSymbol = ofdmflexframegen_writesymbol(fg, frameSamples);
                        symbolLen = ce.numSubcarriers + ce.CPLen;
                        enactScenarioBasebandTx(frameSamples, symbolLen, &ce, &sc);
							
                        // Rx Receives packet
						ofdmflexframesync_execute(fs, frameSamples, symbolLen);
                    } // End Transmition For loop

                    DoneTransmitting = postTxTasks(&ce, &fb, verbose);

					fflush(dataFile);

					// Compute throughput and spectral efficiency
					payload_symbols = (float)ce.payloadLen/(float)ce.bitsPerSym;
					total_symbols = (float)ofdmflexframegen_getframelen(fg);
					throughput = (float)ce.bitsPerSym*ce.bandwidth*(payload_symbols/total_symbols);

					/////////// Print metrics by category /////////////
				
					if(print_frame_info) fprintf(dataFile,"%-10s%-7i","crtsdata",ce.iteration);
					if(print_validity_metrics) fprintf(dataFile,"%-14i%-15i",fb.header_valid,fb.payload_valid);
					if(print_error_metrics) fprintf(dataFile,"%-13i%-12i%-7.2f%-7.2f%-9.2f%-9.2f",fb.payloadByteErrors,fb.payloadBitErrors,ce.PER,ce.BER,ce.PER_avg,ce.BER_avg);
					if(print_signal_quality_metrics) fprintf(dataFile,"%-10.2f%-11.2f",fb.evm,fb.rssi);
					if(print_spectral_metrics) fprintf(dataFile,"%-12.2f%-21.2f",throughput, throughput/ce.bandwidth);
					if(print_goal_metrics) fprintf(dataFile,"%-16.2f",ce.averagedGoalValue);
				    fprintf(dataFile,"\n");

					/////////////////////////////////////////////////////
					
					//All metrics
                    //fprintf(dataFile, "%-10s %-10u %-14i %-15i %-10.2f %-10.2f %-10.2f %-19u %-16.2f %-18u \n", // %-12.2f %-20.2f %-19.2f
					//"crtsdata:", ce.iteration, fb.header_valid, fb.payload_valid, fb.evm, fb.rssi, ce.PER, fb.payloadByteErrors,
					//ce.BER, fb.payloadBitErrors);//, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);
					//Useful metrics
					/*fprintf(dataFile, "%-10s %-10i %-10.2f %-10.2f %-8.2f %-12.2f %-12.2f %-20.2f %-19.2f\n", 
					"crtsdata:", ce.iteration,  fb.evm, fb.rssi, ce.PER,
					ce.BERLastPacket, throughput, throughput/ce.bandwidth, ce.averagedGoalValue);*/

                    // Increment the frame counters and iteration counter
                    ce.frameNumber++;
					ce.iteration++;
                    // Update the clock
                    now = std::clock();
                    ce.runningTime = double(now-begin)/CLOCKS_PER_SEC;

					updateScenarioSummary(&sc_sum, &fb, &ce, i_CE, i_Sc);
                } // End else While loop					
            }

            clock_t end = clock();
            double time = (end-begin)/(double)CLOCKS_PER_SEC + ce.iteration*ce.delay_us/1.0e6;
            //fprintf(dataFile, "Elapsed Time: %f (s)", time);
			fprintf(dataFile, "Begin: %li End: %li Clock/s: %li Time: %f", begin, end, CLOCKS_PER_SEC, time);
            fflush(dataFile);

            // Reset the goal
            ce.latestGoalValue = 0.0;
            ce.errorFreePayloads = 0;
            if (verbose) printf("Scenario %i completed for CE %i.\n", i_Sc+1, i_CE+1);
            fprintf(dataFile, "\n\n");
            fflush(dataFile);

			updateCognitiveEngineSummaryInfo(&ce_sum, &sc_sum, &ce, i_CE, i_Sc);

			// Reset frame number
			ce.frameNumber = 0;
            
        } // End Scenario For loop

        if (verbose) printf("Tests on Cognitive Engine %i completed.\n", i_CE+1);

		// Divide the sum of each metric by the number of scenarios run to get the final metric
		ce_sum.EVM[i_CE] /= i_Sc;
		ce_sum.RSSI[i_CE] /= i_Sc;
		ce_sum.PER[i_CE] /= i_Sc;

		// Print cognitive engine summaries
		printf("Cognitive Engine %i Summary:\nTotal frames: %i\nPercent valid headers: %2f\nPercent valid payloads: %2f\nAverage EVM: %2f\n"
			"Average RSSI: %2f\nAverage BER: %2f\nAverage PER: %2f\n\n", i_CE+1, ce_sum.total_frames[i_CE], (float)ce_sum.valid_headers[i_CE]/(float)ce_sum.total_frames[i_CE],
			(float)ce_sum.valid_payloads[i_CE]/(float)ce_sum.total_frames[i_CE], ce_sum.EVM[i_CE], ce_sum.RSSI[i_CE], (float)ce_sum.bit_errors[i_CE]/(float)ce_sum.total_bits[i_CE], ce_sum.PER[i_CE]);

    } // End CE for loop

	// destroy objects
	msequence_destroy(tx_ms);
	msequence_destroy(rx_ms);
	close(socket_to_server);

	if(!usingUSRPs) close(socket_to_server);

    return 0;
}// End main
