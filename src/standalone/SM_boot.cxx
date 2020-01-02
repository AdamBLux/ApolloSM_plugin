#include <stdio.h>
#include <ApolloSM/ApolloSM.hh>
#include <ApolloSM/ApolloSM_Exceptions.hh>
#include <uhal/uhal.hpp>
#include <vector>
#include <string>
#include <boost/tokenizer.hpp>
#include <unistd.h> // usleep, execl
#include <signal.h>
#include <time.h>

#include <sys/stat.h> //for umask
#include <sys/types.h> //for umask

#include <BUException/ExceptionBase.hh>

#include <boost/program_options.hpp>
#include <fstream>

#define SEC_IN_USEC 1000000
#define NSEC_IN_USEC 1000

#define RUN_DIR "/opt/address_tables/"

// ====================================================================================================
// Definitions

typedef boost::tokenizer<boost::char_separator<char> > tokenizer;

struct temperatures {
  uint8_t MCUTemp;
  uint8_t FIREFLYTemp;
  uint8_t FPGATemp;
  uint8_t REGTemp;
  bool    validData;
};
// ====================================================================================================
// Read from config files and set up all parameters                                                                                                                                                                                         
// For further information see https://theboostcpplibraries.com/boost.program_options

// global polltime variable
int polltime_in_seconds;
#define DEFAULT_POLLTIME_IN_SECONDS 10
#define DEFAULT_POLLTIME_STR "10"

#define CONFIG_FILE "/tmp/bin/SMConfig.txt"

void setup(FILE * logFile) {
  
  try {
    // fileOptions is for parsing config files
    boost::program_options::options_description fileOptions{"File"};
    // Let fileOptions know what information you want it to take out of the config file. Here you want it to take "polltime"
    // Second argument is the type of the information. Second argument has many features, we use the default_value feature. 
    // Third argument is description of the information
    fileOptions.add_options()
      ("polltime", boost::program_options::value<int>()->default_value(DEFAULT_POLLTIME_IN_SECONDS), "amount of time to wait before reading sensors from ApolloSM again");

    // This is a container for the information that fileOptions will get from the config file
    boost::program_options::variables_map vm;

    // Check if config file exists
    std::ifstream ifs{CONFIG_FILE};
    if(ifs) {
      // If config file exists, parse ifs into fileOptions and store information from fileOptions into vm
      // Note that if the config file does not exist, store will not be called and vm will be empty
      fprintf(logFile, "Config file " CONFIG_FILE " exists\n");
      fflush(logFile);
      // Using just parse_config_file without boost::program_options:: works. This may be a boost bug or the compiler may just be very smart
      boost::program_options::store(parse_config_file(ifs, fileOptions), vm);
    } else {
      fprintf(logFile, "Config file " CONFIG_FILE " does not exist\n");
      fflush(logFile);
    }

    // For some reason neither of the following two lines will evaluate to exist. Even if ifs evaluates true in if(ifs) (as it does above), the second line here still will not evaluate to exist.
    // If we can fix either of these two lines we can get rid of the above else statement (not the if, just the else)
    //    fprintf(logFile, "Config file at " CONFIG_FILE " %s\n", !ifs.fail() ? "exists" : "does not exist");
    //    fprintf(logFile, "Config file at " CONFIG_FILE " %s\n", ifs ? "exists" : "does not exist");
    //    fflush(logFile);

    // Notify is not needed but it is powerful. Commeneted out for future references
    //boost::program_options::notify(vm);

    // Check for information in vm
    if(vm.count("polltime")) {
      polltime_in_seconds = vm["polltime"].as<int>();
    }

    std::string polltime_str(std::to_string(polltime_in_seconds));

    fprintf(logFile, "Setting poll time as %s seconds from %s\n", vm.count("polltime") ? polltime_str.c_str() : DEFAULT_POLLTIME_STR, vm.count("polltime") ? "CONFIGURATION FILE" : "DEFAULT VALUE");
    fflush(logFile);

  } catch (const boost::program_options::error &ex) {
    fprintf(logFile, "Caught exception in function setup(): %s \n", ex.what());
    fflush(logFile);
  }
}

// ====================================================================================================
// Kill program if it is in background
bool static volatile loop;

void static signal_handler(int const signum) {
  if(SIGINT == signum || SIGTERM == signum) {
    loop = false;
  }
}

// ====================================================================================================

temperatures sendAndParse(ApolloSM* SM) {
  temperatures temps {0,0,0,0,false};
  std::string recv;

  // read and print
  try{
    recv = (SM->UART_CMD("/dev/ttyUL1", "simple_sensor", '%'));
    temps.validData = true;
  }catch(BUException::IO_ERROR &e){
    //ignore this     
  }
  
  if (temps.validData){
    // Separate by line
    boost::char_separator<char> lineSep("\r\n");
    tokenizer lineTokens{recv, lineSep};

    // One vector for each line 
    std::vector<std::vector<std::string> > allTokens;

    // Separate by spaces
    boost::char_separator<char> space(" ");
    int vecCount = 0;
    // For each line
    for(tokenizer::iterator lineIt = lineTokens.begin(); lineIt != lineTokens.end(); ++lineIt) {
      tokenizer wordTokens{*lineIt, space};
      // We don't yet own any memory in allTokens so we append a blank vector
      std::vector<std::string> blankVec;
      allTokens.push_back(blankVec);
      // One vector per line
      for(tokenizer::iterator wordIt = wordTokens.begin(); wordIt != wordTokens.end(); ++wordIt) {
	allTokens[vecCount].push_back(*wordIt);
      }
      vecCount++;
    }

    // Check for at least one element 
    // Check for two elements in first element
    // Following lines follow the same concept
    std::vector<float> temp_values;
    for(size_t i = 0; 
	i < allTokens.size() && i < 4;
	i++){
      if(2 == allTokens[i].size()) {
	float temp;
	if( (temp = std::atof(allTokens[i][1].c_str())) < 0) {
	  temp = 0;
	}
	temp_values.push_back(temp);
      }
    }
    switch (temp_values.size()){
    case 4:
      temps.REGTemp = (uint8_t)temp_values[3];  
      //fallthrough
    case 3:
      temps.FPGATemp = (uint8_t)temp_values[2];  
      //fallthrough
    case 2:
      temps.FIREFLYTemp = (uint8_t)temp_values[1];  
      //fallthrough
    case 1:
      temps.MCUTemp = (uint8_t)temp_values[0];  
      //fallthrough
      break;
    default:
      break;
    }
  }
  return temps;
}

// ====================================================================================================
void updateTemp(ApolloSM * SM, std::string const & base,uint8_t temp){
  uint32_t oldValues = SM->RegReadRegister(base);
  oldValues = (oldValues & 0xFFFFFF00) | ((temp)&0x000000FF);
  if(0 == temp){    
    SM->RegWriteRegister(base,oldValues);
    return;
  }

  //Update max
  if(temp > (0xFF&(oldValues>>8))){
    oldValues = (oldValues & 0xFFFF00FF) | ((temp<< 8)&0x0000FF00);
  }
  //Update min
  if((temp < (0xFF&(oldValues>>16))) || 
     (0 == (0xFF&(oldValues>>16)))){
    oldValues = (oldValues & 0xFF00FFFF) | ((temp<<16)&0x00FF0000);
  }
  SM->RegWriteRegister(base,oldValues);
}

void sendTemps(ApolloSM* SM, temperatures temps) {
  updateTemp(SM,"SLAVE_I2C.S2.0", temps.MCUTemp);
  updateTemp(SM,"SLAVE_I2C.S3.0", temps.FIREFLYTemp);
  updateTemp(SM,"SLAVE_I2C.S4.0", temps.FPGATemp);
  updateTemp(SM,"SLAVE_I2C.S5.0", temps.REGTemp);
}

// ====================================================================================================


long us_difftime(struct timespec cur, struct timespec end){ 
  return ( (end.tv_sec  - cur.tv_sec )*SEC_IN_USEC + 
	   (end.tv_nsec - cur.tv_nsec)/NSEC_IN_USEC);
}





int main(int, char**) { 

  // ============================================================================
  // Deamon book-keeping
  pid_t pid, sid;
  pid = fork();
  if(pid < 0){
    //Something went wrong.
    //log something
    exit(EXIT_FAILURE);
  }else if(pid > 0){
    //We are the parent and created a child with pid pid
    FILE * pidFile = fopen("/var/run/sm_boot.pid","w");
    fprintf(pidFile,"%d\n",pid);
    fclose(pidFile);
    exit(EXIT_SUCCESS);
  }else{
    // I'm the child!
  }
  
  //Change the file mode mask to allow read/write
  umask(0);

  //Create log file
  FILE * logFile = fopen("/var/log/sm_boot.log","w");
  if(logFile == NULL){
    fprintf(stderr,"Failed to create log file\n");
    exit(EXIT_FAILURE);
  }
  fprintf(logFile,"Opened log file\n");
  fflush(logFile);

  // create nbew SID for the daemon.
  sid = setsid();
  if (sid < 0) {
    fprintf(logFile,"Failed to change SID\n");
    fflush(logFile);
    exit(EXIT_FAILURE);
  }else{
    fprintf(logFile,"Set SID to %d\n",sid);
    fflush(logFile);    
  }

  //Move to RUN_DIR
  if ((chdir(RUN_DIR)) < 0) {
    fprintf(logFile,"Failed to change path to \"" RUN_DIR "\"\n");
    fflush(logFile);
    exit(EXIT_FAILURE);
  }else{
    fprintf(logFile,"Changed path to \"" RUN_DIR "\"\n");
    fflush(logFile);
  }


  //Everything looks good, close the standard file fds.
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  // ============================================================================
  // Read from configuration file and set up parameters
  fprintf(logFile,"Reading from config file now\n");
  fflush(logFile);
  setup(logFile);
  fprintf(logFile,"Finished reading from config file\n");
  fflush(logFile);

  // ============================================================================
  // Daemon code setup

  // ====================================
  // Signal handling
  struct sigaction sa_INT,sa_TERM,old_sa;
  memset(&sa_INT ,0,sizeof(sa_INT)); //Clear struct
  memset(&sa_TERM,0,sizeof(sa_TERM)); //Clear struct
  //setup SA
  sa_INT.sa_handler  = signal_handler;
  sa_TERM.sa_handler = signal_handler;
  sigemptyset(&sa_INT.sa_mask);
  sigemptyset(&sa_TERM.sa_mask);
  sigaction(SIGINT,  &sa_INT , &old_sa);
  sigaction(SIGTERM, &sa_TERM, NULL);
  loop = true;

  // ====================================
  // for counting time
  struct timespec startTS;
  struct timespec stopTS;

  long update_period_us = polltime_in_seconds*SEC_IN_USEC; //sleep time in microseconds



  bool inShutdown = false;
  ApolloSM * SM = NULL;
  try{
    // ==================================
    // Initialize ApolloSM
    SM = new ApolloSM();
    if(NULL == SM){
      fprintf(logFile,"Failed to create new ApolloSM\n");
      fflush(logFile);
    }else{
      fprintf(logFile,"Created new ApolloSM\n");
      fflush(logFile);
    }
    std::vector<std::string> arg;
    arg.push_back("connections.xml");
    SM->Connect(arg);
    //Set the power-up done bit to 1 for the IPMC to read
    SM->RegWriteRegister("SLAVE_I2C.S1.SM.STATUS.DONE",1);    
    fprintf(logFile,"Set STATUS.DONE to 1\n");
    fflush(logFile);
  

    // ====================================
    // Turn on CM uC      
    SM->RegWriteRegister("CM.CM1.CTRL.ENABLE_UC",1);
    fprintf(logFile,"Powering up CM uC\n");
    sleep(1);
  

    // ==================================
    // Main DAEMON loop
    fprintf(logFile,"Starting Monitoring loop\n");
    fflush(logFile);

    uint32_t CM_running = 0;
    while(loop) {
      // loop start time
      clock_gettime(CLOCK_REALTIME, &startTS);

      //=================================
      //Do work
      //=================================

      //Process CM temps
      temperatures temps;  
      if(SM->RegReadRegister("CM.CM1.CTRL.ENABLE_UC")){
	try{
	  temps = sendAndParse(SM);
	}catch(std::exception & e){
	  fprintf(logFile,e.what());
	  //ignoring any exception here for now
	  temps = {0,0,0,0,false};
	}
	
	if(0 == CM_running ){
	  //Drop the non uC temps
	  temps.FIREFLYTemp = 0;
	  temps.FPGATemp = 0;
	  temps.REGTemp = 0;
	}
	CM_running = SM->RegReadRegister("CM.CM1.CTRL.PWR_GOOD");

	sendTemps(SM, temps);
	if(!temps.validData){
	  fprintf(logFile,"Error in parsing data stream\n");
	}
      }else{
	temps = {0,0,0,0,false};
	sendTemps(SM, temps);
      }

      //Check if we are shutting down
      if((!inShutdown) && SM->RegReadRegister("SLAVE_I2C.S1.SM.STATUS.SHUTDOWN_REQ")){
	fprintf(logFile,"Shutdown requested\n");
	inShutdown = true;
	//the IPMC requested a re-boot.
	pid_t reboot_pid;
	if(0 == (reboot_pid = fork())){
	  //Shutdown the system
	  execlp("/sbin/shutdown","/sbin/shutdown","-h","now",NULL);
	  exit(1);
	}
	if(-1 == reboot_pid){
	  inShutdown = false;
	  fprintf(logFile,"Error! fork to shutdown failed!\n");
	}else{
	  //Shutdown the command module (if up)
	  SM->PowerDownCM(1,5);
	}
      }
      //=================================


      // monitoring sleep
      clock_gettime(CLOCK_REALTIME, &stopTS);
      // sleep for 10 seconds minus how long it took to read and send temperature    
      useconds_t sleep_us = update_period_us - us_difftime(startTS, stopTS);
      if(sleep_us > 0){
	usleep(sleep_us);
      }
    }
  }catch(BUException::exBase const & e){
    fprintf(logFile,"Caught BUException: %s\n   Info: %s\n",e.what(),e.Description());
    fflush(logFile);      
  }catch(std::exception const & e){
    fprintf(logFile,"Caught std::exception: %s\n",e.what());
    fflush(logFile);      
  }


  //make sure the CM is off
  //Shutdown the command module (if up)
  SM->PowerDownCM(1,5);
  SM->RegWriteRegister("CM.CM1.CTRL.ENABLE_UC",0);

  
  //If we are shutting down, do the handshanking.
  if(inShutdown){
    fprintf(logFile,"Tell IPMC we have shut-down\n");
    //We are no longer booted
    SM->RegWriteRegister("SLAVE_I2C.S1.SM.STATUS.DONE",0);
    //we are shut down
    //    SM->RegWriteRegister("SLAVE_I2C.S1.SM.STATUS.SHUTDOWN",1);
    // one last HB
    //PS heartbeat
    SM->RegReadRegister("SLAVE_I2C.HB_SET1");
    SM->RegReadRegister("SLAVE_I2C.HB_SET2");

  }
  
  //Clean up
  if(NULL != SM) {
    delete SM;
  }
  
  // Restore old action of receiving SIGINT (which is to kill program) before returning 
  sigaction(SIGINT, &old_sa, NULL);
  fprintf(logFile,"SM boot Daemon ended\n");
  fclose(logFile);
  
  return 0;
}
