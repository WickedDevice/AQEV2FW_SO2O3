#include <Wire.h>
#include <SPI.h>
#include <WildFire.h>
#include <WildFire_CC3000.h>
#include <TinyWatchdog.h>
#include <SHT25.h>
#include <MCP342x.h>
#include <LMP91000.h>
#include <WildFire_SPIFlash.h>
#include <Time.h>
#include <CapacitiveSensor.h>
#include <LiquidCrystal.h>
#include <PubSubClient.h>
#include <util/crc16.h>

// semantic versioning - see http://semver.org/
#define AQEV2FW_MAJOR_VERSION 2
#define AQEV2FW_MINOR_VERSION 0
#define AQEV2FW_PATCH_VERSION 0

WildFire wf;
WildFire_CC3000 cc3000;
TinyWatchdog tinywdt;
LMP91000 lmp91000;
MCP342x adc;
SHT25 sht25;
WildFire_SPIFlash flash;
CapacitiveSensor touch = CapacitiveSensor(A0, A1);
LiquidCrystal lcd(A3, A2, 4, 5, 6, 8);
byte mqtt_server[4] = { 0 };    
PubSubClient mqtt_client;
char mqtt_client_id[32] = {0};
WildFire_CC3000_Client wifiClient;

unsigned long current_millis = 0;
char firmware_version[16] = {0};
float temperature_degc = 0.0f;
float relative_humidity_percent = 0.0f;
float no2_ppb = 0.0f;
float co_ppm = 0.0f;

// samples are buffered approximately every two seconds
#define NO2_SAMPLE_BUFFER_DEPTH (32)
float no2_sample_buffer[NO2_SAMPLE_BUFFER_DEPTH] = {0};

#define CO_SAMPLE_BUFFER_DEPTH (32)
float co_sample_buffer[CO_SAMPLE_BUFFER_DEPTH] = {0};

#define TEMPERATURE_SAMPLE_BUFFER_DEPTH (16)
float temperature_sample_buffer[TEMPERATURE_SAMPLE_BUFFER_DEPTH] = {0};

#define HUMIDITY_SAMPLE_BUFFER_DEPTH (16)
float humidity_sample_buffer[HUMIDITY_SAMPLE_BUFFER_DEPTH] = {0};

#define TOUCH_SAMPLE_BUFFER_DEPTH (4)
float touch_sample_buffer[TOUCH_SAMPLE_BUFFER_DEPTH] = {0};

#define LCD_ERROR_MESSAGE_DELAY   (4000)
#define LCD_SUCCESS_MESSAGE_DELAY (2000)

boolean no2_ready = false;
boolean co_ready = false;
boolean temperature_ready = false;
boolean humidity_ready = false;

boolean init_sht25_ok = false;
boolean init_co_afe_ok = false;
boolean init_no2_afe_ok = false;
boolean init_co_adc_ok = false;
boolean init_no2_adc_ok = false;
boolean init_spi_flash_ok = false;
boolean init_cc3000_ok = false;

// the software's operating mode
#define MODE_CONFIG      (1)
#define MODE_OPERATIONAL (2)
// submodes of normal behavior
#define SUBMODE_NORMAL   (3)
#define SUBMODE_ZEROING  (4)

uint8_t mode = MODE_OPERATIONAL;

// the config mode state machine's return values
#define CONFIG_MODE_NOTHING_SPECIAL  (0)
#define CONFIG_MODE_GOT_INIT         (1)
#define CONFIG_MODE_GOT_EXIT         (2)

#define EEPROM_MAC_ADDRESS    (E2END + 1 - 6)    // MAC address, i.e. the last 6-bytes of EEPROM
// more parameters follow, address relative to each other so they don't overlap
#define EEPROM_CONNECT_METHOD     (EEPROM_MAC_ADDRESS - 1)        // connection method encoded as a single byte value 
#define EEPROM_SSID               (EEPROM_CONNECT_METHOD - 32)    // ssid string, up to 32 characters (one of which is a null terminator)
#define EEPROM_NETWORK_PWD        (EEPROM_SSID - 32)              // network password, up to 32 characters (one of which is a null terminator)
#define EEPROM_SECURITY_MODE      (EEPROM_NETWORK_PWD - 1)        // security mode encoded as a single byte value, consistent with the CC3000 library
#define EEPROM_STATIC_IP_ADDRESS  (EEPROM_SECURITY_MODE - 4)      // static ipv4 address, 4 bytes - 0.0.0.0 indicates use DHCP
#define EEPROM_STATIC_NETMASK     (EEPROM_STATIC_IP_ADDRESS - 4)  // static netmask, 4 bytes
#define EEPROM_STATIC_GATEWAY     (EEPROM_STATIC_NETMASK - 4)     // static default gateway ip address, 4 bytes
#define EEPROM_STATIC_DNS         (EEPROM_STATIC_GATEWAY - 4)     // static dns server ip address, 4 bytes
#define EEPROM_MQTT_PASSWORD      (EEPROM_STATIC_DNS - 32)        // password for mqtt server, up to 32 characters (one of which is a null terminator)
#define EEPROM_NO2_SENSITIVITY    (EEPROM_MQTT_PASSWORD - 4)      // float value, 4-bytes, the sensitivity from the sticker
#define EEPROM_NO2_CAL_SLOPE      (EEPROM_NO2_SENSITIVITY - 4)    // float value, 4-bytes, the slope applied to the sensor
#define EEPROM_NO2_CAL_OFFSET     (EEPROM_NO2_CAL_SLOPE - 4)      // float value, 4-btyes, the offset applied to the sensor
#define EEPROM_CO_SENSITIVITY     (EEPROM_NO2_CAL_OFFSET - 4)     // float value, 4-bytes, the sensitivity from the sticker
#define EEPROM_CO_CAL_SLOPE       (EEPROM_CO_SENSITIVITY - 4)     // float value, 4-bytes, the slope applied to the sensor
#define EEPROM_CO_CAL_OFFSET      (EEPROM_CO_CAL_SLOPE - 4)       // float value, 4-btyes, the offset applied to the sensor
#define EEPROM_PRIVATE_KEY        (EEPROM_CO_CAL_OFFSET - 32)     // 32-bytes of Random Data (256-bits)
#define EEPROM_MQTT_SERVER_NAME   (EEPROM_PRIVATE_KEY - 32)       // string, the DNS name of the MQTT server (default opensensors.io), up to 32 characters (one of which is a null terminator)
#define EEPROM_MQTT_USERNAME      (EEPROM_MQTT_SERVER_NAME - 32)  // string, the user name for the MQTT server (default airqualityegg), up to 32 characters (one of which is a null terminator)
#define EEPROM_MQTT_CLIENT_ID     (EEPROM_MQTT_USERNAME - 32)     // string, the client identifier for the MQTT server (default SHT25 identifier), between 1 and 23 characters long
#define EEPROM_MQTT_AUTH          (EEPROM_MQTT_CLIENT_ID - 1)     // MQTT authentication enabled, single byte value 0 = disabled or 1 = enabled
#define EEPROM_MQTT_PORT          (EEPROM_MQTT_AUTH - 4)          // MQTT authentication enabled, reserve four bytes, even though you only need two for a port
#define EEPROM_UPDATE_SERVER_NAME (EEPROM_MQTT_PORT - 32)         // string, the DNS name of the Firmware Update server (default update.wickeddevice.com), up to 32 characters (one of which is a null terminator)
#define EEPROM_OPERATIONAL_MODE   (EEPROM_UPDATE_SERVER_NAME - 1) // operational mode encoded as a single byte value (e.g. NORMAL, ZEROING, etc.)
//  /\
//   L Add values up here by subtracting offsets to previously added values
//   * ... and make sure the addresses don't collide and start overlapping!
//   T Add values down here by adding offsets to previously added values
//  \/
#define EEPROM_BACKUP_PRIVATE_KEY (EEPROM_BACKUP_CO_CAL_OFFSET + 4)
#define EEPROM_BACKUP_CO_CAL_OFFSET (EEPROM_BACKUP_CO_CAL_SLOPE + 4)
#define EEPROM_BACKUP_CO_CAL_SLOPE (EEPROM_BACKUP_CO_SENSITIVITY + 4)
#define EEPROM_BACKUP_CO_SENSITIVITY (EEPROM_BACKUP_NO2_CAL_OFFSET + 4)
#define EEPROM_BACKUP_NO2_CAL_OFFSET (EEPROM_BACKUP_NO2_CAL_SLOPE + 4)
#define EEPROM_BACKUP_NO2_CAL_SLOPE (EEPROM_BACKUP_NO2_SENSITIVITY + 4)
#define EEPROM_BACKUP_NO2_SENSITIVITY (EEPROM_BACKUP_MQTT_PASSWORD + 32)
#define EEPROM_BACKUP_MQTT_PASSWORD (EEPROM_BACKUP_MAC_ADDRESS + 6)
#define EEPROM_BACKUP_MAC_ADDRESS (EEPROM_BACKUP_CHECK + 2) // backup parameters are added here offset from the EEPROM_CRC_CHECKSUM
#define EEPROM_BACKUP_CHECK   (EEPROM_CRC_CHECKSUM + 2) // 2-byte value with various bits set if backup has ever happened
#define EEPROM_CRC_CHECKSUM   (E2END + 1 - 1024) // reserve the last 1kB for config
// the only things that need "backup" are those which are unique to a device
// other things can have "defaults" stored in flash (i.e. using the restore defaults command)

// valid connection methods
// only DIRECT is supported initially
#define CONNECT_METHOD_DIRECT        (0)
#define CONNECT_METHOD_SMARTCONFIG   (1)
#define CONNECT_METHOD_PFOD          (2)

// backup status bits
#define BACKUP_STATUS_MAC_ADDRESS_BIT       (7)
#define BACKUP_STATUS_MQTT_PASSSWORD_BIT    (6)
#define BACKUP_STATUS_NO2_CALIBRATION_BIT   (5)
#define BACKUP_STATUS_CO_CALIBRATION_BIT    (4)
#define BACKUP_STATUS_PRIVATE_KEY_BIT       (3)

#define BIT_IS_CLEARED(val, b) (!(val & (1UL << b)))
#define CLEAR_BIT(val, b) \
  do { \
    val &= ~(1UL << b); \
  } while(0)

void help_menu(char * arg);
void print_eeprom_value(char * arg);
void initialize_eeprom_value(char * arg);
void restore(char * arg);
void set_mac_address(char * arg);
void set_connection_method(char * arg);
void set_ssid(char * arg);
void set_network_password(char * arg);
void set_network_security_mode(char * arg);
void set_static_ip_address(char * arg);
void use_command(char * arg);
void set_mqtt_password(char * arg);
void set_mqtt_server(char * arg);
void set_mqtt_port(char * arg);
void set_mqtt_username(char * arg);
void set_mqtt_client_id(char * arg);
void set_mqtt_authentication(char * arg);
void backup(char * arg);
void set_no2_slope(char * arg);
void set_no2_offset(char * arg);
void set_no2_sensitivity(char * arg);
void set_co_slope(char * arg);
void set_co_offset(char * arg);
void set_co_sensitivity(char * arg);
void set_private_key(char * arg);
void set_operational_mode(char * arg);

// Note to self:
//   When implementing a new parameter, ask yourself:
//     should there be a command for the user to set its value directly
//     should 'get' support it (almost certainly the answer is yes)
//     should 'init' support it (is there a way to set it without user intervention)
//     should 'restore' support it directly
//     should 'restore defaults' support it
//   ... and anytime you do the above, remember to update the help_menu
//   ... and remember, anything that changes the config EEPROM
//       needs to call recomputeAndStoreConfigChecksum after doing so

// the order of the command keywords in this array
// must be kept in index-correspondence with the associated
// function pointers in the command_functions array
//
// these keywords are padded with spaces
// in order to ease printing as a table
// string comparisons should use strncmp rather than strcmp
char * commands[] = {
  "get       ",
  "init      ",
  "restore   ",
  "mac       ",
  "method    ",
  "ssid      ",
  "pwd       ",
  "security  ",
  "staticip  ",
  "use       ",
  "mqttsrv   ",
  "mqttport  ",
  "mqttuser  ",
  "mqttpwd   ",  
  "mqttid    ",
  "mqttauth  ",
  "updatesrv ",
  "backup    ",
  "no2_cal   ",
  "no2_slope ",
  "no2_off   ",
  "co_cal    ",
  "co_slope  ",
  "co_off    ",
  "key       ",
  "opmode    ",
  0
};

void (*command_functions[])(char * arg) = {
  print_eeprom_value,
  initialize_eeprom_value,
  restore,
  set_mac_address,
  set_connection_method,
  set_ssid,
  set_network_password,
  set_network_security_mode,
  set_static_ip_address,
  use_command,
  set_mqtt_server,
  set_mqtt_port,  
  set_mqtt_username,
  set_mqtt_password,  
  set_mqtt_client_id,
  set_mqtt_authentication,
  set_update_server_name,
  backup,
  set_no2_sensitivity,
  set_no2_slope,
  set_no2_offset,
  set_co_sensitivity,
  set_co_slope,
  set_co_offset,
  set_private_key,
  set_operational_mode,
  0
};

// tiny watchdog timer intervals
unsigned long previous_tinywdt_millis = 0;
const long tinywdt_interval = 1000;

// mqtt publish timer intervals
unsigned long previous_mqtt_publish_millis = 0;
const long mqtt_publish_interval = 5000;

// sensor sampling timer intervals
unsigned long previous_sensor_sampling_millis = 0;
const long sensor_sampling_interval = 2000;

// touch sampling timer intervals
unsigned long previous_touch_sampling_millis = 0;
const long touch_sampling_interval = 200;

// progress dots timer intervals
unsigned long previous_progress_dots_millis = 0;
const long progress_dots_interval = 1000;

// zero check timer intervals
unsigned long previous_zero_check_millis = 0;
const long zero_check_interval = 5000;

#define NUM_HEARTBEAT_WAVEFORM_SAMPLES (84)
const uint8_t heartbeat_waveform[NUM_HEARTBEAT_WAVEFORM_SAMPLES] PROGMEM = {
  95, 94, 95, 96, 95, 94, 95, 96, 95, 94,
  95, 96, 95, 94, 95, 96, 95, 97, 105, 112,
  117, 119, 120, 117, 111, 103, 95, 94, 95, 96,
  95, 94, 100, 131, 162, 193, 224, 255, 244, 214,
  183, 152, 121, 95, 88, 80, 71, 74, 82, 90, 
  96, 95, 94, 95, 96, 97, 106, 113, 120, 125, 
  129, 132, 133, 131, 128, 124, 118, 111, 103, 96,
  95, 96, 95, 94, 95, 96, 95, 94, 95, 99, 
  105, 106, 101, 96  
};
uint8_t heartbeat_waveform_index = 0;

void setup() {
  boolean integrity_check_passed = false;
  boolean valid_ssid_passed = false;
  
  // initialize hardware
  initializeHardware(); 
  backlightOff();
  
  integrity_check_passed = checkConfigIntegrity();
  valid_ssid_passed = valid_ssid_config();  
  uint8_t target_mode = eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE);  
  boolean ok_to_exit_config_mode = true;  
  
  // config mode processing loop
  do{
    // check for initial integrity of configuration in eeprom
    if(!integrity_check_passed) {
      Serial.println(F("Info: Config memory integrity check failed, automatically falling back to CONFIG mode."));
      configInject("aqe\r");
      Serial.println();
      setLCD_P(PSTR("CONFIG INTEGRITY"
                    "  CHECK FAILED  "));
      mode = MODE_CONFIG;
    }
    else if(mode_requires_wifi(target_mode) && !valid_ssid_passed){
      Serial.println(F("Info: No valid SSID configured, automatically falling back to CONFIG mode."));
      configInject("aqe\r");
      Serial.println();
      setLCD_P(PSTR("   VALID SSID   "
                    "    REQUIRED    "));      
      mode = MODE_CONFIG;
    }
    else {
      // if the appropriate escape sequence is received within 8 seconds
      // go into config mode
      const long startup_time_period = 12000;
      long start = millis();
      long min_over = 100;
      boolean got_serial_input = false;
      Serial.println(F("Enter 'aqe' for CONFIG mode."));
      Serial.print(F("OPERATIONAL mode automatically begins after "));
      Serial.print(startup_time_period / 1000);
      Serial.println(F(" secs of no input."));
      setLCD_P(PSTR("CONNECT TERMINAL"
                    "FOR CONFIG MODE "));
                    
      current_millis = millis();
      while (current_millis < start + startup_time_period) { // can get away with this sort of thing at start up
        current_millis = millis();
        
        if(current_millis - previous_touch_sampling_millis >= touch_sampling_interval){
          previous_touch_sampling_millis = current_millis;    
          collectTouch();    
          processTouchQuietly();  
        }      
      
        if (Serial.available()) {
          if (got_serial_input == false) {
            Serial.println();
          }
          got_serial_input = true;
  
          start = millis(); // reset the timeout
          if (CONFIG_MODE_GOT_INIT == configModeStateMachine(Serial.read(), false)) {
            mode = MODE_CONFIG;
            break;
          }
        }
  
        // output a countdown to the Serial Monitor
        if (millis() - start >= min_over) {
          uint8_t countdown_value_display = (startup_time_period - 500 - min_over) / 1000;
          if (got_serial_input == false) {
            Serial.print(countdown_value_display);
            Serial.print(F("..."));
          }
          
          updateCornerDot();
          
          min_over += 1000;
        }
      }
    }
    Serial.println();
  
    if (mode == MODE_CONFIG) {
      const uint32_t idle_timeout_period_ms = 1000UL * 60UL * 5UL; // 5 minutes
      uint32_t idle_time_ms = 0;
      Serial.println(F("-~=* In CONFIG Mode *=~-"));
      if(integrity_check_passed && valid_ssid_passed){
        setLCD_P(PSTR("  CONFIG MODE"));
      }
      
      Serial.print(F("OPERATIONAL mode begins automatically after "));
      Serial.print((idle_timeout_period_ms / 1000UL) / 60UL);
      Serial.println(F(" mins without input."));
      Serial.println(F("Enter 'help' for a list of available commands, "));
      Serial.println(F("      ...or 'help <cmd>' for help on a specific command"));
  
      configInject("get settings\r");
      Serial.println();
      Serial.println(F(" @=============================================================@"));
      Serial.println(F(" # GETTING STARTED                                             #"));
      Serial.println(F(" #-------------------------------------------------------------#"));
      Serial.println(F(" #   First type 'ssid your_ssid_here' and & press <enter>      #"));
      Serial.println(F(" #   Then type 'pwd your_network_password' & press <enter>     #"));
      Serial.println(F(" #   Then type 'get settings' & press <enter> to review config #"));
      Serial.println(F(" #   Finally, type 'exit' to go into OPERATIONAL mode,         #"));
      Serial.println(F(" #     and verify that the Egg connects to your network!       #")); 
      Serial.println(F(" @=============================================================@"));
  
      prompt();
      for (;;) {
        current_millis = millis();
  
        if(current_millis - previous_touch_sampling_millis >= touch_sampling_interval){
          previous_touch_sampling_millis = current_millis;    
          collectTouch();    
          processTouchQuietly();  
        }
  
        // stuck in this loop until the command line receives an exit command
        if (Serial.available()) {
          idle_time_ms = 0;
          // if you get serial traffic, pass it along to the configModeStateMachine for consumption
          if (CONFIG_MODE_GOT_EXIT == configModeStateMachine(Serial.read(), false)) {
            break;
          }
        }
  
        // pet the watchdog once a ssecond
        if (current_millis - previous_tinywdt_millis >= tinywdt_interval) {
          idle_time_ms += tinywdt_interval;
          petWatchdog();
          previous_tinywdt_millis = current_millis;
        }
  
        if (idle_time_ms >= idle_timeout_period_ms) {
          Serial.println(F("Info: Idle time expired, exiting CONFIG mode."));
          break;
        }
      }
    }
    
    integrity_check_passed = checkConfigIntegrity();
    valid_ssid_passed = valid_ssid_config();    
    ok_to_exit_config_mode = true;
    
    target_mode = eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE);      
       
    if(!integrity_check_passed){
      ok_to_exit_config_mode = false;
    }
    else if(mode_requires_wifi(target_mode) && !valid_ssid_passed){
      ok_to_exit_config_mode = false;
    }
    
  } while(!ok_to_exit_config_mode);

  Serial.println(F("-~=* In OPERATIONAL Mode *=~-"));
  setLCD_P(PSTR("OPERATIONAL MODE"));
  delay(LCD_SUCCESS_MESSAGE_DELAY);
  
  // ... but *which* operational mode are we in?
  mode = target_mode;
  
  if(mode_requires_wifi(mode)){
    // Try and Connect to the Configured Network
    if(!restartWifi()){
      Serial.println(F("Error: Failed to connect to configured network. Rebooting."));
      Serial.flush();
      watchdogForceReset();
    }
  
    petWatchdog();
  
    // Check for Firmware Updates 
  
    // Connect to MQTT server
    if(!mqttReconnect()){
      Serial.print(F("Error: Unable to connect to MQTT server"));
      Serial.flush();
      watchdogForceReset();    
    }
    
    petWatchdog();
  }
  
  if(mode == SUBMODE_NORMAL){
    setLCD_P(PSTR("TEMP ---  RH ---"
                  "NO2  ---  CO ---"));           
    delay(LCD_SUCCESS_MESSAGE_DELAY);                          
  }
  else{
    setLCD_P(PSTR("ZERO-ING SENSORS"
                  "NO2  ---  CO ---"));           
    delay(LCD_SUCCESS_MESSAGE_DELAY);                              
  }
}

void loop() {
  current_millis = millis();

  if(current_millis - previous_sensor_sampling_millis >= sensor_sampling_interval){
    previous_sensor_sampling_millis = current_millis;    
    Serial.print(F("Info: Sampling Sensors @ "));
    Serial.println(millis());
    collectNO2();
    collectCO();
    collectTemperature();
    collectHumidity();  
  }

  if(current_millis - previous_touch_sampling_millis >= touch_sampling_interval){
    previous_touch_sampling_millis = current_millis;    
    collectTouch();    
    processTouchQuietly();  
  }  

  // the following loop routines *must* return reasonably frequently
  // so that the watchdog timer is serviced
  switch(mode){
    case SUBMODE_NORMAL:
      loop_wifi_mqtt_mode();
      break;
    case SUBMODE_ZEROING: 
      if(loop_zeroing_mode()){
        mode = eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE);  
        if(mode != SUBMODE_NORMAL){
          eeprom_write_byte((uint8_t *) EEPROM_OPERATIONAL_MODE, SUBMODE_NORMAL);       
        }
        watchdogForceReset();
      }
      break;
    default: // unkown operating mode, nothing to be done 
      break;
  }
  
  // pet the watchdog
  if (current_millis - previous_tinywdt_millis >= tinywdt_interval) {
    previous_tinywdt_millis = current_millis;
    Serial.println(F("Info: Watchdog Pet."));
    petWatchdog();
  }
}

/****** INITIALIZATION SUPPORT FUNCTIONS ******/
void init_firmware_version(void){
  snprintf(firmware_version, 15, "%d.%d.%d", 
    AQEV2FW_MAJOR_VERSION, 
    AQEV2FW_MINOR_VERSION, 
    AQEV2FW_PATCH_VERSION);
}

void initializeHardware(void) {
  wf.begin();
  Serial.begin(115200);

  init_firmware_version();

  Serial.println(F(" +------------------------------------+"));
  Serial.println(F(" |   Welcome to Air Quality Egg 2.0   |"));
  Serial.print(F(" |       Firmware Version "));
  Serial.print(firmware_version);
  Serial.println(F("       |"));
  Serial.println(F(" +------------------------------------+"));
  Serial.println();

  pinMode(A6, OUTPUT);
  backlightOn();

  // smiley face
  byte smiley[8] = {
          B00000,
          B00000,
          B01010,
          B00000,
          B10001,
          B01110,
          B00000,
          B00000
  };
  
  byte frownie[8] = {
          B00000,
          B00000,
          B01010,
          B00000,
          B01110,
          B10001,
          B00000,
          B00000
  };
  
  lcd.createChar(0, smiley);
  lcd.createChar(1, frownie);  
  lcd.begin(16, 2);
  setLCD_P(PSTR("AIR QUALITY EGG "));
  char tmp[17] = {0};
  snprintf(tmp, 16, "VERSION %d.%d.%d", 
    AQEV2FW_MAJOR_VERSION, 
    AQEV2FW_MINOR_VERSION,
    AQEV2FW_PATCH_VERSION);
    
  updateLCD(tmp, 1);
  
  Wire.begin();

  // Initialize slot select pins
  Serial.print(F("Info: Slot Select Pins Initialization..."));
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT);
  selectNoSlot();
  Serial.println(F("OK."));

  // Initialize Tiny Watchdog
  Serial.print(F("Info: Tiny Watchdog Initialization..."));
  watchdogInitialize();
  Serial.println(F("OK."));
  
  // Initialize SPI Flash
  Serial.print(F("Info: SPI Flash Initialization..."));
  if (flash.initialize()) {
    Serial.println(F("OK."));
    init_spi_flash_ok = true;
  }
  else {
    Serial.println(F("Fail."));
    init_spi_flash_ok = false;
  }

  // Initialize SHT25
  Serial.print(F("Info: SHT25 Initization..."));
  if (sht25.begin()) {
    Serial.println(F("OK."));
    init_sht25_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_sht25_ok = false;
  }

  // Initialize NO2 Sensor
  Serial.print(F("Info: NO2 Sensor AFE Initization..."));
  selectSlot2();
  if (lmp91000.configure(
        LMP91000_TIA_GAIN_350K | LMP91000_RLOAD_10OHM,
        LMP91000_REF_SOURCE_EXT | LMP91000_INT_Z_67PCT
        | LMP91000_BIAS_SIGN_NEG | LMP91000_BIAS_8PCT,
        LMP91000_FET_SHORT_DISABLED | LMP91000_OP_MODE_AMPEROMETRIC)) {
    Serial.println(F("OK."));
    init_no2_afe_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_no2_afe_ok = false;
  }

  Serial.print(F("Info: NO2 Sensor ADC Initization..."));
  if(MCP342x::errorNone == adc.convert(MCP342x::channel1, MCP342x::oneShot, MCP342x::resolution16, MCP342x::gain1)){
    Serial.println(F("OK."));
    init_no2_adc_ok = true;    
  }
  else{
    Serial.println(F("Failed."));
    init_no2_adc_ok = false;    
  }

  Serial.print(F("Info: CO Sensor AFE Initization..."));
  selectSlot1();
  if (lmp91000.configure(
        LMP91000_TIA_GAIN_350K | LMP91000_RLOAD_10OHM,
        LMP91000_REF_SOURCE_EXT | LMP91000_INT_Z_20PCT
        | LMP91000_BIAS_SIGN_POS | LMP91000_BIAS_1PCT,
        LMP91000_FET_SHORT_DISABLED | LMP91000_OP_MODE_AMPEROMETRIC)) {
    Serial.println(F("OK."));
    init_co_afe_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_co_afe_ok = false;
  }
  
  Serial.print(F("Info: CO Sensor ADC Initization..."));
  if(MCP342x::errorNone == adc.convert(MCP342x::channel1, MCP342x::oneShot, MCP342x::resolution16, MCP342x::gain1)){
    Serial.println(F("OK."));
    init_co_adc_ok = true;    
  }
  else{
    Serial.println(F("Failed."));
    init_co_adc_ok = false;    
  }

  selectNoSlot();

  Serial.print(F("Info: CC3000 Initialization..."));
  if (cc3000.begin()) {
    Serial.println(F("OK."));
    init_cc3000_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_cc3000_ok = false;
  }
}

/****** CONFIGURATION SUPPORT FUNCTIONS ******/
boolean checkConfigIntegrity(void) {
  uint16_t computed_crc = computeConfigChecksum();
  uint16_t stored_crc = eeprom_read_word((const uint16_t *) EEPROM_CRC_CHECKSUM);
  if (computed_crc == stored_crc) {
    return true;
  }
  else {
    //Serial.print(F("Computed CRC = "));
    //Serial.print(computed_crc, HEX);
    //Serial.print(F(", Stored CRC = "));
    //Serial.println(stored_crc, HEX);
    return false;
  }
}

// this state machine receives bytes and
// returns true if the function is in config mode
uint8_t configModeStateMachine(char b, boolean reset_buffers) {
  static boolean received_init_code = false;
  const uint8_t buf_max_write_idx = 126; // [63] must always have a null-terminator
  static char buf[128] = {0}; // buffer to hold commands / data
  static uint8_t buf_idx = 0;  // current number of bytes in buf
  boolean line_terminated = false;
  char * first_arg = 0;
  uint8_t ret = CONFIG_MODE_NOTHING_SPECIAL;

  if (reset_buffers) {
    buf_idx = 0;
  }

  //  Serial.print('[');
  //  if(isprint(b)) Serial.print((char) b);
  //  Serial.print(']');
  //  Serial.print('\t');
  //  Serial.print("0x");
  //  if(b < 0x10) Serial.print('0');
  //  Serial.println(b, HEX);

  // the following logic rejects all non-printable characters besides 0D, 0A, and 7F
  if (b == 0x7F) { // backspace key is special
    if (buf_idx > 0) {
      buf_idx--;
      buf[buf_idx] = '\0';
      Serial.print(b); // echo the character
    }
  }
  else if (b == 0x0D || b == 0x0A) { // carriage return or new line is also special
    buf[buf_idx] = '\0'; // force terminator do not advance write pointer
    line_terminated = true;
    Serial.println(); // echo the character
  }
  else if ((buf_idx <= buf_max_write_idx) && isprint(b)) {
    // otherwise if there's space and the character is 'printable' add it to the buffer
    // silently drop all other non-printable characters
    buf[buf_idx++] = b;
    buf[buf_idx] = '\0';
    Serial.print(b); // echo the character
  }

  char lower_buf[128] = {0};
  if (line_terminated) {
    strncpy(lower_buf, buf, 127);
    lowercase(lower_buf);
  }

  // process the data currently stored in the buffer
  if (received_init_code && line_terminated) {
    // with the exeption of the command "exit"
    // commands are always of the form <command> <argument>
    // they are minimally parsed here and delegated to
    // callback functions that take the argument as a string

    // Serial.print("buf = ");
    // Serial.println(buf);

    if (strncmp("exit", lower_buf, 4) == 0) {
      Serial.println(F("Exiting CONFIG mode..."));
      ret = CONFIG_MODE_GOT_EXIT;
    }
    else {
      // the string must have one, and only one, space in it
      uint8_t num_spaces = 0;
      char * p;
      for (p = buf; *p != '\0'; p++) { // all lines are terminated by '\r' above
        if (*p == ' ') {
          num_spaces++;
        }

        if ((num_spaces == 1) && (*p == ' ')) {
          // if this is the first space encountered, null the original string here
          // in order to mark the first argument string
          *p = '\0';
        }
        else if ((num_spaces > 0) && (first_arg == 0) && (*p != ' ')) {
          // if we are beyond the first space,
          // and have not encountered the beginning of the first argument
          // and this character is not a space, it is by definition
          // the beginning of the first argument, so mark it as such
          first_arg = p;
        }
      }

      // deal with commands that can legitimately have no arguments first
      if (strncmp("help", lower_buf, 4) == 0) {
        help_menu(first_arg);
      }
      else if (first_arg != 0) {
        //Serial.print(F("Received Command: \""));
        //Serial.print(buf);
        //Serial.print(F("\" with Argument: \""));
        //Serial.print(first_arg);
        //Serial.print(F("\""));
        //Serial.println();

        // command with argument was received, determine if it's valid
        // and if so, call the appropriate command processing function
        boolean command_found = false;
        for (uint8_t ii = 0; commands[ii] != 0; ii++) {
          if (strncmp(commands[ii], lower_buf, strlen(buf)) == 0) {
            command_functions[ii](first_arg);
            command_found = true;
            break;
          }
        }

        if (!command_found) {
          Serial.print(F("Error: Unknown command \""));
          Serial.print(buf);
          Serial.println(F("\""));
        }
      }
      else if (strlen(buf) > 0) {
        Serial.print(F("Error: Argument expected for command \""));
        Serial.print(buf);
        Serial.println(F("\", but none was received"));
      }
    }
  }
  else if (line_terminated) {
    // before we receive the init code, the only things
    // we are looking for are an exact match to the strings
    // "AQE\r" or "aqe\r"

    if (strncmp("aqe", lower_buf, 3) == 0) {
      received_init_code = true;
      ret = CONFIG_MODE_GOT_INIT;
    }
    else if (strlen(buf) > 0) {
      Serial.print(F("Error: Expecting Config Mode Unlock Code (\"aqe\"), but received \""));
      Serial.print(buf);
      Serial.println(F("\""));
    }
  }

  // clean up the buffer if you got a line termination
  if (line_terminated) {
    if (ret == CONFIG_MODE_NOTHING_SPECIAL) {
      prompt();
    }
    buf[0] = '\0';
    buf_idx = 0;
  }

  return ret;
}

void prompt(void) {
  Serial.print(F("AQE>: "));
}

// command processing function implementations
void configInject(char * str) {
  boolean reset_buffers = true;
  while (*str != '\0') {
    configModeStateMachine(*str++, reset_buffers);
    if (reset_buffers) {
      reset_buffers = false;
    }
  }
}

void lowercase(char * str) {
  uint8_t len = strlen(str);
  if (len < 255) { // guard against an infinite loop
    for (uint8_t ii = 0; ii < len; ii++) {
      str[ii] = tolower(str[ii]);
    }
  }
}

void note_know_what_youre_doing(){
  Serial.println(F("   note:    Unless you *really* know what you're doing, you should"));
  Serial.println(F("            probably not be using this command."));  
}

void warn_could_break_upload(){
  Serial.println(F("   warning: Using this command incorrectly can prevent your device"));
  Serial.println(F("            from publishing data to the internet."));  
}

void warn_could_break_connect(){
  Serial.println(F("   warning: Using this command incorrectly can prevent your device"));
  Serial.println(F("            from connecting to your network."));  
}

void help_menu(char * arg) {
  const uint8_t commands_per_line = 3;
  const uint8_t first_dynamic_command_index = 2;

  lowercase(arg);

  if (arg == 0) {
    // list the commands that are legal
    Serial.print(F("help    \texit    \t"));
    for (uint8_t ii = 0, jj = first_dynamic_command_index; commands[ii] != 0; ii++, jj++) {
      if ((jj % commands_per_line) == 0) {
        Serial.println();
      }
      //Serial.print(jj + 1);
      //Serial.print(". ");
      Serial.print(commands[ii]);
      Serial.print('\t');
    }
    Serial.println();
  }
  else {
    // we have an argument, so the user is asking for some specific usage instructions
    // as they pertain to this command
    if (strncmp("help", arg, 4) == 0) {
      Serial.println(F("help <param>"));
      Serial.println(F("   <param> is any legal command keyword"));
      Serial.println(F("   result: usage instructions are printed"));
      Serial.println(F("           for the command named <arg>"));
    }
    else if (strncmp("exit", arg, 4) == 0) {
      Serial.println(F("exit"));
      Serial.println(F("   exits CONFIG mode and begins OPERATIONAL mode."));
    }
    else if (strncmp("get", arg, 3) == 0) {
      Serial.println(F("get <param>"));
      Serial.println(F("   <param> is one of:"));
      Serial.println(F("      settings - displays all viewable settings"));
      Serial.println(F("      mac - the MAC address of the cc3000"));
      Serial.println(F("      method - the Wi-Fi connection method"));
      Serial.println(F("      ssid - the Wi-Fi SSID to connect to"));
      Serial.println(F("      pwd - lol, sorry, that's not happening!"));
      Serial.println(F("      security - the Wi-Fi security mode"));
      Serial.println(F("      ipmode - the Wi-Fi IP-address mode"));
      Serial.println(F("      mqttsrv - MQTT server name"));
      Serial.println(F("      mqttport - MQTT server port"));           
      Serial.println(F("      mqttuser - MQTT username"));
      Serial.println(F("      mqttpwd - lol, sorry, that's not happening either!"));      
      Serial.println(F("      mqttid - MQTT client ID"));      
      Serial.println(F("      mqttauth - MQTT authentication enabled?"));      
      Serial.println(F("      updatesrv - Update server name"));      
      Serial.println(F("      no2_cal - NO2 sensitivity [nA/ppm]"));
      Serial.println(F("      no2_slope - NO2 sensors slope [ppb/V]"));
      Serial.println(F("      no2_off - NO2 sensors offset [V]"));
      Serial.println(F("      co_cal - CO sensitivity [nA/ppm]"));
      Serial.println(F("      co_slope - CO sensors slope [ppm/V]"));
      Serial.println(F("      co_off - CO sensors offset [V]"));
      Serial.println(F("      key - lol, sorry, that's also not happening!"));
      Serial.println(F("      opmode - the Operational Mode the Egg is configured for"));
      Serial.println(F("   result: the current, human-readable, value of <param>"));
      Serial.println(F("           is printed to the console."));
    }
    else if (strncmp("init", arg, 4) == 0) {
      Serial.println(F("init <param>"));
      Serial.println(F("   <param> is one of:"));
      Serial.println(F("      mac - retrieves the mac address from"));
      Serial.println(F("            the CC3000 and stores it in EEPROM"));
    }
    else if (strncmp("restore", arg, 7) == 0) {
      Serial.println(F("restore <param>"));
      Serial.println(F("   <param> is one of:"));
      Serial.println(F("      defaults - performs 'method direct'"));
      Serial.println(F("                 performs 'security wpa2'"));
      Serial.println(F("                 performs 'use dhcp'"));
      Serial.println(F("                 performs 'opmode normal'"));
      Serial.println(F("                 performs 'mqttsrv opensensors.io'"));
      Serial.println(F("                 performs 'mqttport 1883'"));           
      Serial.println(F("                 performs 'mqttauth enable'"));        
      Serial.println(F("                 performs 'mqttuser airqualityegg'"));  
      Serial.println(F("                 performs 'restore mac'"));
      Serial.println(F("                 performs 'restore mqttpwd'"));
      Serial.println(F("                 performs 'restore mqttid'"));      
      Serial.println(F("                 performs 'restore updatesrv'"));      
      Serial.println(F("                 performs 'restore key'"));
      Serial.println(F("                 performs 'restore no2'"));
      Serial.println(F("                 performs 'restore co'"));
      Serial.println(F("                 clears the SSID from memory"));
      Serial.println(F("                 clears the Network Password from memory"));
      Serial.println(F("      mac      - retrieves the mac address from BACKUP"));
      Serial.println(F("                 and assigns it to the CC3000, via a 'mac' command"));
      Serial.println(F("      mqttpwd  - restores the MQTT password from BACKUP "));
      Serial.println(F("      mqttid   - restores the MQTT client ID"));    
      Serial.println(F("      updatesrv- restores the Update server name"));          
      Serial.println(F("      key      - restores the Private Key from BACKUP "));
      Serial.println(F("      no2      - restores the NO2 calibration parameters from BACKUP "));
      Serial.println(F("      co       - restores the CO calibration parameters from BACKUP "));
    }
    else if (strncmp("mac", arg, 3) == 0) {
      Serial.println(F("mac <address>"));
      Serial.println(F("   <address> is a MAC address of the form:"));
      Serial.println(F("                08:ab:73:DA:8f:00"));
      Serial.println(F("   result:  The entered MAC address is assigned to the CC3000"));
      Serial.println(F("            and is stored in the EEPROM."));
      warn_could_break_connect();
    }
    else if (strncmp("method", arg, 6) == 0) {
      Serial.println(F("method <type>"));
      Serial.println(F("   <type> is one of:"));
      Serial.println(F("      direct - use parameters entered in CONFIG mode"));
      Serial.println(F("      smartconfig - use smart config process [not yet supported]"));
      Serial.println(F("      pfod - use pfodWifiConnect config process  [not yet supported]"));
      warn_could_break_connect();      
    }
    else if (strncmp("ssid", arg, 4) == 0) {
      Serial.println(F("ssid <string>"));
      Serial.println(F("   <string> is the SSID of the network the device should connect to."));
      warn_could_break_connect();      
    }
    else if (strncmp("pwd", arg, 3) == 0) {
      Serial.println(F("pwd <string>"));
      Serial.println(F("   <string> is the network password for "));
      Serial.println(F("      the SSID that the device should connect to."));
      warn_could_break_connect();      
    }
    else if (strncmp("security", arg, 8) == 0) {
      Serial.println(F("security <mode>"));
      Serial.println(F("   <mode> is one of:"));
      Serial.println(F("      open - the network is unsecured"));
      Serial.println(F("      wep  - the network WEP security"));
      Serial.println(F("      wpa  - the network WPA Personal security"));
      Serial.println(F("      wpa2 - the network WPA2 Personal security"));
      warn_could_break_connect();      
    }
    else if (strncmp("staticip", arg, 8) == 0) {
      Serial.println(F("staticip <config>"));
      Serial.println(F("   <config> is four ip address separated by spaces"));
      Serial.println(F("      <param1> static ip address, e.g. 192.168.1.17"));
      Serial.println(F("      <param2> netmask, e.g. 255.255.255.0"));      
      Serial.println(F("      <param3> default gateway ip address, e.g. 192.168.1.1"));      
      Serial.println(F("      <param4> dns server ip address, e.g. 8.8.8.8"));      
      Serial.println(F("   result: The entered static network parameters will be used by the CC3000"));
      Serial.println(F("   note:   To configure DHCP use command 'use dhcp'"));
      warn_could_break_connect();      
    }
    else if (strncmp("use", arg, 3) == 0) {
      Serial.println(F("use <param>"));
      Serial.println(F("   <param> is one of:"));
      Serial.println(F("      dhcp - wipes the Static IP address from the EEPROM"));
      warn_could_break_connect();      
    }
    else if (strncmp("mqttpwd", arg, 7) == 0) {
      Serial.println(F("mqttpwd <string>"));
      Serial.println(F("   <string> is the password the device will use to connect "));
      Serial.println(F("      to the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();      
    }
    else if (strncmp("mqttsrv", arg, 7) == 0) {
      Serial.println(F("mqttsrv <string>"));
      Serial.println(F("   <string> is the DNS name of the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }
    else if (strncmp("mqttuser", arg, 8) == 0) {
      Serial.println(F("mqttuser <string>"));
      Serial.println(F("   <string> is the username used to connect to the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }
    else if (strncmp("mqttid", arg, 6) == 0) {
      Serial.println(F("mqttid <string>"));
      Serial.println(F("   <string> is the Client ID used to connect to the MQTT server."));
      Serial.println(F("            Must be between 1 and 23 characters long per MQTT v3.1 spec."));    
      // Ref: http://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html#connect  
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }    
    else if (strncmp("mqttauth", arg, 8) == 0) {
      Serial.println(F("mqttauth <string>"));
      Serial.println(F("   <string> is the one of 'enable' or 'disable'"));
      note_know_what_youre_doing();
      warn_could_break_upload();   
    }        
    else if (strncmp("mqttport", arg, 8) == 0) {
      Serial.println(F("mqttport <number>"));
      Serial.println(F("   <number> is the a number between 1 and 65535 inclusive"));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }            
    else if (strncmp("updatesrv", arg, 9) == 0) {
      Serial.println(F("updatesrv <string>"));
      Serial.println(F("   <string> is the DNS name of the Update server."));
      Serial.println(F("   note:    Unless you *really* know what you're doing, you should"));
      Serial.println(F("            probably not be using this command."));
      Serial.println(F("   warning: Using this command incorrectly can prevent your device"));
      Serial.println(F("            from getting firmware updates over the internet."));
    }    
    else if (strncmp("backup", arg, 3) == 0) {
      Serial.println(F("backup <param>"));
      Serial.println(F("   <param> is one of:"));
      Serial.println(F("      mqttpwd  - backs up the MQTT password"));
      Serial.println(F("      mac      - backs up the CC3000 MAC address"));
      Serial.println(F("      key      - backs up the 256-bit private key"));
      Serial.println(F("      no2      - backs up the NO2 calibration parameters"));
      Serial.println(F("      co       - backs up the CO calibration parameters"));
      Serial.println(F("      all      - does all of the above"));
    }
    else if (strncmp("no2_cal", arg, 7) == 0) {
      Serial.println(F("no2_cal <number>"));
      Serial.println(F("   <number> is the decimal value of NO2 sensitivity [nA/ppm]"));
      Serial.println(F("   note: also sets the NO2 slope based on the sensitivity"));
    }
    else if (strncmp("no2_slope", arg, 9) == 0) {
      Serial.println(F("no2_slope <number>"));
      Serial.println(F("   <number> is the decimal value of NO2 sensor slope [ppb/V]"));
    }
    else if (strncmp("no2_off", arg, 7) == 0) {
      Serial.println(F("no2_off <number>"));
      Serial.println(F("   <number> is the decimal value of NO2 sensor offset [V]"));
    }
    else if (strncmp("co_cal", arg, 6) == 0) {
      Serial.println(F("co_cal <number>"));
      Serial.println(F("   <number> is the decimal value of CO sensitivity [nA/ppm]"));
      Serial.println(F("   note: also sets the CO slope based on the sensitivity"));      
    }
    else if (strncmp("co_slope", arg, 8) == 0) {
      Serial.println(F("co_slope <number>"));
      Serial.println(F("   <number> is the decimal value of CO sensor slope [ppm/V]"));
    }
    else if (strncmp("co_off", arg, 6) == 0) {
      Serial.println(F("co_off <number>"));
      Serial.println(F("   <number> is the decimal value of CO sensor offset [V]"));
    }
    else if (strncmp("key", arg, 3) == 0) {
      Serial.println(F("key <string>"));
      Serial.println(F("   <string> is a 64-character string representing "));
      Serial.println(F("      a 32-byte (256-bit) hexadecimal value of the private key"));
    }
    else if (strncmp("opmode", arg, 6) == 0) {
      Serial.println(F("opmode <mode>"));
      Serial.println(F("   <mode> is one of:"));
      Serial.println(F("      normal - publish data to MQTT server over Wi-Fi"));
      Serial.println(F("      zero - perform sensor zero-ing function, and resume normal mode when complete."));
      Serial.println(F("             Note: This process may take several hours to complete from cold start."));      
    }    
    else {
      Serial.print(F("Error: There is no help available for command \""));
      Serial.print(arg);
      Serial.println(F("\""));
    }
  }
}

void print_eeprom_mac(void) {
  uint8_t _mac_address[6] = {0};
  // retrieve the value from EEPROM
  eeprom_read_block(_mac_address, (const void *) EEPROM_MAC_ADDRESS, 6);

  // print the stored value, formatted
  for (uint8_t ii = 0; ii < 6; ii++) {
    if (_mac_address[ii] < 0x10) {
      Serial.print(F("0"));
    }
    Serial.print(_mac_address[ii], HEX);

    // only print colons after the first 5 values
    if (ii < 5) {
      Serial.print(F(":"));
    }
  }
  Serial.println();
}

void print_eeprom_connect_method(void) {
  uint8_t method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);
  switch (method) {
    case CONNECT_METHOD_DIRECT:
      Serial.println(F("Direct Connect"));
      break;
    case CONNECT_METHOD_SMARTCONFIG:
      Serial.println(F("Smart Config Connect [not currently supported]"));
      break;
    case CONNECT_METHOD_PFOD:
      Serial.println(F("Pfod Wi-Fi Connect [not currently supported]"));
      break;
    default:
      Serial.print(F("Error: Unknown connection method code [0x"));
      if (method < 0x10) {
        Serial.print(F("0"));
      }
      Serial.print(method, HEX);
      Serial.println(F("]"));
      break;
  }
}

boolean valid_ssid_config(void) {
  char ssid[32] = {0};
  boolean ssid_contains_only_printables = true;

  eeprom_read_block(ssid, (const void *) EEPROM_SSID, 31);
  for (uint8_t ii = 0; ii < 32; ii++) {
    if (ssid[ii] == '\0') {
      break;
    }
    else if (!isprint(ssid[ii])) {
      ssid_contains_only_printables = false;
      break;
    }
  }

  if (!ssid_contains_only_printables || (strlen(ssid) == 0)) {
    return false;
  }

  return true;
}

void print_eeprom_ssid(void) {
  char ssid[32] = {0};
  eeprom_read_block(ssid, (const void *) EEPROM_SSID, 31);

  if (!valid_ssid_config()) {
    Serial.println(F("No SSID currently configured."));
  }
  else {
    Serial.println(ssid);
  }
}

void print_eeprom_security_type(void) {
  uint8_t security = eeprom_read_byte((const uint8_t *) EEPROM_SECURITY_MODE);
  switch (security) {
    case WLAN_SEC_UNSEC:
      Serial.println(F("Open"));
      break;
    case WLAN_SEC_WEP:
      Serial.println(F("WEP"));
      break;
    case WLAN_SEC_WPA:
      Serial.println(F("WPA"));
      break;
    case WLAN_SEC_WPA2:
      Serial.println(F("WPA2"));
      break;
    default:
      Serial.print(F("Error: Unknown security mode code [0x"));
      if (security < 0x10) {
        Serial.print(F("0"));
      }
      Serial.print(security, HEX);
      Serial.println(F("]"));
      break;
  }
}

void print_eeprom_ipmode(void) {
  uint8_t ip[4] = {0};
  uint8_t netmask[4] = {0};
  uint8_t gateway[4] = {0};
  uint8_t dns[4] = {0};
  uint8_t noip[4] = {0};
  eeprom_read_block(ip, (const void *) EEPROM_STATIC_IP_ADDRESS, 4);
  eeprom_read_block(netmask, (const void *) EEPROM_STATIC_NETMASK, 4);
  eeprom_read_block(gateway, (const void *) EEPROM_STATIC_GATEWAY, 4);
  eeprom_read_block(dns, (const void *) EEPROM_STATIC_DNS, 4);
  
  if (memcmp(ip, noip, 4) == 0) {
    Serial.println(F("Configured for DHCP"));
  }
  else {
    Serial.println(F("Configured for Static IP: "));
    for(uint8_t param_idx = 0; param_idx < 4; param_idx++){         
      for (uint8_t ii = 0; ii < 4; ii++) {
        switch(param_idx){
          case 0:
            if(ii == 0){
              Serial.print(F("   IP Address:      "));
            }
            Serial.print(ip[ii], DEC);
            break;
          case 1:
            if(ii == 0){
              Serial.print(F("   Netmask:         "));
            }
            Serial.print(netmask[ii], DEC);
            break;
          case 2:
            if(ii == 0){
              Serial.print(F("   Default Gateway: "));
            }
            Serial.print(gateway[ii], DEC);
            break;
          case 3:
            if(ii == 0){
              Serial.print(F("   DNS Server:      "));
            }         
            Serial.print(dns[ii], DEC); 
            break;
        }   
        
        if( ii != 3 ){        
          Serial.print(F("."));
        }
        else{
          Serial.println(); 
        }
      }      
    }
  }
}

void print_eeprom_float(const float * address) {
  float val = eeprom_read_float(address);
  Serial.println(val, 9);
}

void print_label_with_star_if_not_backed_up(char * label, uint8_t bit_number) {
  uint16_t backup_check = eeprom_read_word((const uint16_t *) EEPROM_BACKUP_CHECK);
  Serial.print(F("  "));
  if (!BIT_IS_CLEARED(backup_check, bit_number)) {
    Serial.print(F("*"));
  }
  else {
    Serial.print(F(" "));
  }
  Serial.print(F(" "));
  Serial.print(label);
}

void print_eeprom_string(const char * address){
  char tmp[32] = {0};
  eeprom_read_block(tmp, (const void *) address, 31);
  Serial.println(tmp);
}

void print_eeprom_update_server(){
  print_eeprom_string((const char *) EEPROM_UPDATE_SERVER_NAME);
}

void print_eeprom_mqtt_server(){
  print_eeprom_string((const char *) EEPROM_MQTT_SERVER_NAME);
}


void print_eeprom_mqtt_client_id(){
  print_eeprom_string((const char *) EEPROM_MQTT_CLIENT_ID);
}

void print_eeprom_mqtt_username(){
  print_eeprom_string((const char *) EEPROM_MQTT_USERNAME);
}

void print_eeprom_mqtt_authentication(){
  uint8_t auth = eeprom_read_byte((uint8_t *) EEPROM_MQTT_AUTH);
  if(auth){
    Serial.println(F("    MQTT Authentication: Enabled"));    
    Serial.print(F("    MQTT Username: "));
    print_eeprom_mqtt_username();              
  }
  else{
    Serial.println(F("    MQTT Authentication: Disabled"));
  }
}

void print_eeprom_operational_mode(uint8_t opmode){   
  switch (opmode) {
    case SUBMODE_NORMAL:
      Serial.println(F("Normal"));
      break;
    case SUBMODE_ZEROING:
      Serial.println(F("Zeroing"));
      break;
    default:
      Serial.print(F("Error: Unknown operational mode [0x"));
      if (opmode < 0x10) {
        Serial.print(F("0"));
      }
      Serial.print(opmode, HEX);
      Serial.println(F("]"));
      break;
  }  
}

void print_eeprom_value(char * arg) {
  if (strncmp(arg, "mac", 3) == 0) {
    print_eeprom_mac();
  }
  else if (strncmp(arg, "method", 6) == 0) {
    print_eeprom_connect_method();
  }
  else if (strncmp(arg, "ssid", 4) == 0) {
    print_eeprom_ssid();
  }
  else if (strncmp(arg, "security", 8) == 0) {
    print_eeprom_security_type();
  }
  else if (strncmp(arg, "ipmode", 6) == 0) {
    print_eeprom_ipmode();
  }
  else if (strncmp(arg, "no2_cal", 7) == 0) {
    print_eeprom_float((const float *) EEPROM_NO2_SENSITIVITY);
  }
  else if (strncmp(arg, "no2_slope", 9) == 0) {
    print_eeprom_float((const float *) EEPROM_NO2_CAL_SLOPE);
  }
  else if (strncmp(arg, "no2_off", 7) == 0) {
    print_eeprom_float((const float *) EEPROM_NO2_CAL_OFFSET);
  }
  else if (strncmp(arg, "co_cal", 6) == 0) {
    print_eeprom_float((const float *) EEPROM_CO_SENSITIVITY);
  }
  else if (strncmp(arg, "co_slope", 8) == 0) {
    print_eeprom_float((const float *) EEPROM_CO_CAL_SLOPE);
  }
  else if (strncmp(arg, "co_off", 6) == 0) {
    print_eeprom_float((const float *) EEPROM_CO_CAL_OFFSET);
  }
  else if(strncmp(arg, "mqttsrv", 7) == 0) {
    print_eeprom_string((const char *) EEPROM_MQTT_SERVER_NAME);    
  }
  else if(strncmp(arg, "mqttport", 8) == 0) {
    Serial.println(eeprom_read_dword((const uint32_t *) EEPROM_MQTT_PORT));      
  }  
  else if(strncmp(arg, "mqttuser", 8) == 0) {
    print_eeprom_string((const char *) EEPROM_MQTT_USERNAME);    
  }  
  else if(strncmp(arg, "mqttid", 6) == 0) {
    print_eeprom_string((const char *) EEPROM_MQTT_CLIENT_ID);    
  }
  else if(strncmp(arg, "mqttauth", 8) == 0) {
    Serial.println(eeprom_read_byte((const uint8_t *) EEPROM_MQTT_AUTH));    
  }
  else if(strncmp(arg, "opmode", 6) == 0) {
     print_eeprom_operational_mode(eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE));
  }
  else if (strncmp(arg, "settings", 8) == 0) {
    char allff[64] = {0};
    memset(allff, 0xff, 64);

    // print all the settings to the screen in an orderly fashion
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Operational Mode:                                           |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.print(F("    "));
    print_eeprom_operational_mode(eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE));
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Network Settings:                                           |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
    print_label_with_star_if_not_backed_up("MAC Address: ", BACKUP_STATUS_MAC_ADDRESS_BIT);
    print_eeprom_mac();
    Serial.print(F("    SSID: "));
    print_eeprom_ssid();
    Serial.print(F("    Security Mode: "));
    print_eeprom_security_type();
    Serial.print(F("    IP Mode: "));
    print_eeprom_ipmode();
    Serial.print(F("    Update Server: "));
    print_eeprom_update_server();    
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | MQTT Settings:                                              |"));
    Serial.println(F(" +-------------------------------------------------------------+"));    
    Serial.print(F("    MQTT Server: "));
    print_eeprom_mqtt_server();   
    Serial.print(F("    MQTT Port: "));
    Serial.println(eeprom_read_dword((const uint32_t *) EEPROM_MQTT_PORT)); 
    Serial.print(F("    MQTT Client ID: "));
    print_eeprom_mqtt_client_id();       
    print_eeprom_mqtt_authentication(); 
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Credentials:                                                |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
    print_label_with_star_if_not_backed_up("MQTT Password backed up? [* means no]", BACKUP_STATUS_MQTT_PASSSWORD_BIT);
    Serial.println();
    print_label_with_star_if_not_backed_up("Private key backed up? [* means no]", BACKUP_STATUS_PRIVATE_KEY_BIT);
    Serial.println();
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Sensor Calibrations:                                        |"));
    Serial.println(F(" +-------------------------------------------------------------+"));

    print_label_with_star_if_not_backed_up("NO2 Sensitivity [nA/ppm]: ", BACKUP_STATUS_NO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_NO2_SENSITIVITY);
    print_label_with_star_if_not_backed_up("NO2 Slope [ppb/V]: ", BACKUP_STATUS_NO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_NO2_CAL_SLOPE);
    print_label_with_star_if_not_backed_up("NO2 Offset [V]: ", BACKUP_STATUS_NO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_NO2_CAL_OFFSET);

    print_label_with_star_if_not_backed_up("CO Sensitivity [nA/ppm]: ", BACKUP_STATUS_CO_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_CO_SENSITIVITY);
    print_label_with_star_if_not_backed_up("CO Slope [ppm/V]: ", BACKUP_STATUS_CO_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_CO_CAL_SLOPE);
    print_label_with_star_if_not_backed_up("CO Offset [V]: ", BACKUP_STATUS_CO_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_CO_CAL_OFFSET);

    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | note: '*' next to label means the setting is not backed up. |"));
    Serial.println(F(" |     run 'backup all' when you are satisfied                 |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
  }
  else {
    Serial.print(F("Error: Unexpected Variable Name \""));
    Serial.print(arg);
    Serial.println(F("\""));
  }
}

// goes into the CC3000 and stores the
// MAC address from it in the EEPROM
void initialize_eeprom_value(char * arg) {
  if (strncmp(arg, "mac", 3) == 0) {
    uint8_t _mac_address[6];
    if (!cc3000.getMacAddress(_mac_address)) {
      Serial.println(F("Error: Could not retrieve MAC address from CC3000"));
    }
    else {
      eeprom_write_block(_mac_address, (void *) EEPROM_MAC_ADDRESS, 6);
      recomputeAndStoreConfigChecksum();
    }
  }
  else {
    Serial.print(F("Error: Unexpected Variable Name \""));
    Serial.print(arg);
    Serial.println(F("\""));
  }
}

void restore(char * arg) {
  char blank[32] = {0};
  uint8_t tmp[32] = {0};
  boolean valid = true;

  // things that must have been backed up before restoring.
  // 1. MAC address              0x80
  // 2. MQTT Password            0x40
  // 3. Private Key              0x20
  // 4. NO2 Calibration Values   0x10
  // 5. CO Calibratino Values    0x80

  uint16_t backup_check = eeprom_read_word((const uint16_t *) EEPROM_BACKUP_CHECK);

  if (strncmp(arg, "defaults", 8) == 0) {
    prompt();
    configInject("method direct\r");
    configInject("security wpa2\r");
    configInject("use dhcp\r");
    configInject("opmode normal\r");
    configInject("mqttsrv opensensors.io\r");
    configInject("mqttport 1883\r");        
    configInject("mqttauth enable\r");    
    configInject("mqttuser airqualityegg\r");
    configInject("restore mqttpwd\r");
    configInject("restore mqttid\r");
    configInject("restore updatesrv\r");
    configInject("restore key\r");
    configInject("restore no2\r");
    configInject("restore co\r");
    configInject("restore mac\r");

    eeprom_write_block(blank, (void *) EEPROM_SSID, 32); // clear the SSID
    eeprom_write_block(blank, (void *) EEPROM_NETWORK_PWD, 32); // clear the Network Password
    Serial.println();
  }
  else if (strncmp(arg, "mac", 3) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_MAC_ADDRESS_BIT)) {
      Serial.println(F("Error: MAC address must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    uint8_t _mac_address[6] = {0};
    char setmac_string[32] = {0};
    eeprom_read_block(_mac_address, (const void *) EEPROM_BACKUP_MAC_ADDRESS, 6);
    snprintf(setmac_string, 31,
             "mac %02x:%02x:%02x:%02x:%02x:%02x\r",
             _mac_address[0],
             _mac_address[1],
             _mac_address[2],
             _mac_address[3],
             _mac_address[4],
             _mac_address[5]);

    configInject(setmac_string);
    Serial.println();
  }
  else if (strncmp("mqttpwd", arg, 7) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_MQTT_PASSSWORD_BIT)) {
      Serial.println(F("Error: MQTT Password must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_MQTT_PASSWORD, 32);
    eeprom_write_block(tmp, (void *) EEPROM_MQTT_PASSWORD, 32);
  }
  else if (strncmp("mqttid", arg, 6) == 0) {
    // get the 8-byte unique electronic ID from the SHT25 
    // convert it to a string, and store it to EEPROM
    uint8_t serial_number[8];
    sht25.getSerialNumber(serial_number);
    snprintf((char *) tmp, 31, "egg%02X%02X%02X%02X%02X%02X%02X%02X",
      serial_number[0],
      serial_number[1],
      serial_number[2],
      serial_number[3],
      serial_number[4],
      serial_number[5],
      serial_number[6],
      serial_number[7]);
    eeprom_write_block(tmp, (void *) EEPROM_MQTT_CLIENT_ID, 32);
  }  
  else if (strncmp("updatesrv", arg, 9) == 0) {
    eeprom_write_block("update.wickeddevice.com", (void *) EEPROM_UPDATE_SERVER_NAME, 32);
  }  
  else if (strncmp("key", arg, 3) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_PRIVATE_KEY_BIT)) {
      Serial.println(F("Error: Private key must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_PRIVATE_KEY, 32);
    eeprom_write_block(tmp, (void *) EEPROM_PRIVATE_KEY, 32);
  }
  else if (strncmp("no2", arg, 6) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_NO2_CALIBRATION_BIT)) {
      Serial.println(F("Error: NO2 calibration must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_NO2_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_NO2_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_NO2_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_NO2_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_NO2_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_NO2_CAL_OFFSET, 4);
  }
  else if (strncmp("co", arg, 5) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_CO_CALIBRATION_BIT)) {
      Serial.println(F("Error: CO calibration must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_CO_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_CO_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_CO_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_CO_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_CO_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_CO_CAL_OFFSET, 4);
  }
  else {
    valid = false;
    Serial.print(F("Error: Unexpected paramater name \""));
    Serial.print(arg);
    Serial.println(F("\""));
  }

  if (valid) {
    recomputeAndStoreConfigChecksum();
  }

}

void set_mac_address(char * arg) {
  uint8_t _mac_address[6] = {0};
  char tmp[32] = {0};
  strncpy(tmp, arg, 31); // copy the string so you don't mutilate the argument
  char * token = strtok(tmp, ":");
  uint8_t num_tokens = 0;

  // parse the argument string, expected to be of the form ab:01:33:51:c8:77
  while (token != NULL) {
    if ((strlen(token) == 2) && isxdigit(token[0]) && isxdigit(token[1]) && (num_tokens < 6)) {
      _mac_address[num_tokens++] = (uint8_t) strtoul(token, NULL, 16);
    }
    else {
      Serial.print(F("Error: MAC address parse error on input \""));
      Serial.print(arg);
      Serial.println(F("\""));
      return; // return early
    }
    token = strtok(NULL, ":");
  }

  if (num_tokens == 6) {
    if (!cc3000.setMacAddress(_mac_address)) {
      Serial.println(F("Error: Failed to write MAC address to CC3000"));
    }
    else { // cc3000 mac address accepted
      eeprom_write_block(_mac_address, (void *) EEPROM_MAC_ADDRESS, 6);
      recomputeAndStoreConfigChecksum();
    }
  }
  else {
    Serial.println(F("Error: MAC address must contain 6 bytes, with each separated by ':'"));
  }
}

void set_connection_method(char * arg) {
  lowercase(arg);
  boolean valid = true;
  if (strncmp(arg, "direct", 6) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_CONNECT_METHOD, CONNECT_METHOD_DIRECT);
  }
  else if (strncmp(arg, "smartconfig", 11) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_CONNECT_METHOD, CONNECT_METHOD_SMARTCONFIG);
  }
  else if (strncmp(arg, "pfod", 4) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_CONNECT_METHOD, CONNECT_METHOD_PFOD);
  }
  else {
    Serial.print(F("Error: Invalid connection method entered - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    Serial.println(F("       valid options are: 'direct', 'smartconfig', and 'pfod'"));
    valid = false;
  }

  if (valid) {
    recomputeAndStoreConfigChecksum();
  }
}

void set_ssid(char * arg) {
  // we've reserved 32-bytes of EEPROM for an SSID
  // so the argument's length must be <= 31
  char ssid[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(ssid, arg, len);
    eeprom_write_block(ssid, (void *) EEPROM_SSID, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: SSID must be less than 32 characters in length"));
  }
}

void set_network_password(char * arg) {
  // we've reserved 32-bytes of EEPROM for a network password
  // so the argument's length must be <= 31
  char password[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(password, arg, len);
    eeprom_write_block(password, (void *) EEPROM_NETWORK_PWD, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: Network password must be less than 32 characters in length"));
  }
}

void set_network_security_mode(char * arg) {
  boolean valid = true;
  if (strncmp("open", arg, 4) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_SECURITY_MODE, WLAN_SEC_UNSEC);
  }
  else if (strncmp("wep", arg, 3) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_SECURITY_MODE, WLAN_SEC_WEP);
  }
  else if (strncmp("wpa2", arg, 4) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_SECURITY_MODE, WLAN_SEC_WPA2);
  }
  else if (strncmp("wpa", arg, 3) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_SECURITY_MODE, WLAN_SEC_WPA);
  }
  else {
    Serial.print(F("Error: Invalid security mode entered - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    Serial.println(F("       valid options are: 'open', 'wep', 'wpa', and 'wpa2'"));
    valid = false;
  }

  if (valid) {
    recomputeAndStoreConfigChecksum();
  }
}

void set_operational_mode(char * arg) {
  boolean valid = true;
  if (strncmp("normal", arg, 6) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_OPERATIONAL_MODE, SUBMODE_NORMAL);
  }
  else if (strncmp("zero", arg, 4) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_OPERATIONAL_MODE, SUBMODE_ZEROING);
  }
  else {
    Serial.print(F("Error: Invalid operational mode entered - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    Serial.println(F("       valid options are: 'normal', 'zero'"));
    valid = false;
  }

  if(valid) {
    recomputeAndStoreConfigChecksum();
  }
}

void set_static_ip_address(char * arg) {
  uint8_t _ip_address[4] = {0};
  uint8_t _gateway_ip[4] = {0};
  uint8_t _dns_ip[4] = {0}; 
  uint8_t _netmask[4] = {0};
  
  char tmp[128] = {0};
  strncpy(tmp, arg, 127); // copy the string so you don't mutilate the argument
  char * params[4] = {0};
  uint8_t param_idx = 0;
  
  // first tokenize on spaces, you should end up with four strings     
  char * token = strtok(tmp, " ");
  uint8_t num_tokens = 0;

  while (token != NULL) {
    if(param_idx > 3){
      Serial.println(F("Error: Too many parameters passed to staticip"));
      Serial.print(F("       "));
      Serial.println(arg);
      configInject("help staticip\r");
      Serial.println();
      return;
    }
    params[param_idx++] = token;   
    token = strtok(NULL, " "); 
  }
  
  if(param_idx != 4){
     Serial.println(F("Error: Too few parameters passed to staticip"));
     Serial.print(F("       "));
     Serial.println(arg);
     configInject("help staticip\r");   
     Serial.println();     
     return;
  }

  for(param_idx = 0; param_idx < 4; param_idx++){
    token = strtok(params[param_idx], ".");
    num_tokens = 0;    
    
    // parse the parameter string, expected to be of the form 192.168.1.52
    while (token != NULL) {
      uint8_t tokenlen = strlen(token);
      if ((tokenlen < 4) && (num_tokens < 4)) {
        for (uint8_t ii = 0; ii < tokenlen; ii++) {
          if (!isdigit(token[ii])) {
            Serial.print(F("Error: IP address octets must be integer values [@param "));
            Serial.print(param_idx + 1);
            Serial.println(F("]"));
            return;
          }
        }
        uint32_t octet = (uint8_t) strtoul(token, NULL, 10);
        if (octet < 256) {
          switch(param_idx){
            case 0:
              _ip_address[num_tokens++] = octet;
              break;
            case 1:
              _netmask[num_tokens++] = octet;
              break;
            case 2:
              _gateway_ip[num_tokens++] = octet;
              break;
            case 3:
              _dns_ip[num_tokens++] = octet;
              break; 
            default:
              break;            
          }
        }
        else {
          Serial.print(F("Error: IP address octets must be between 0 and 255 inclusive [@param "));
          Serial.print(param_idx + 1);
          Serial.println(F("]"));
          return;
        }
      }
      else {
        Serial.print(F("Error: IP address parse error on input \""));
        Serial.print(token);
        Serial.println(F("\""));
        return; 
      }
      
      token = strtok(NULL, ".");
    }    
      
    if (num_tokens != 4){
      Serial.print(F("Error: IP Address must contain 4 valid octets separated by '.' [@param "));
      Serial.print(param_idx + 1);
      Serial.println(F("]"));
      return;
    }

  }

  // if we got this far, it means we got 4 valid IP addresses, and they
  // are stored in their respective local variables    
  uint32_t ipAddress = cc3000.IP2U32(_ip_address[0], _ip_address[1], _ip_address[2], _ip_address[3]);
  uint32_t netMask = cc3000.IP2U32(_netmask[0], _netmask[1], _netmask[2], _netmask[3]);
  uint32_t defaultGateway = cc3000.IP2U32(_gateway_ip[0], _gateway_ip[1], _gateway_ip[2], _gateway_ip[3]);
  uint32_t dns = cc3000.IP2U32(_dns_ip[0], _dns_ip[1], _dns_ip[2], _dns_ip[3]);  
  
  //   Note that the setStaticIPAddress function will save its state
  //   in the CC3000's internal non-volatile memory and the details
  //   will be used the next time the CC3000 connects to a network.
  //   This means you only need to call the function once and the
  //   CC3000 will remember the connection details.   
  
  if (!cc3000.setStaticIPAddress(ipAddress, netMask, defaultGateway, dns)) {
    Serial.println(F("Error: setStaticIPAddress Failed on CC3000"));
    return;          
  }  
  else{
    eeprom_write_block(_ip_address, (void *) EEPROM_STATIC_IP_ADDRESS, 4);  
    eeprom_write_block(_netmask, (void *) EEPROM_STATIC_NETMASK, 4);
    eeprom_write_block(_gateway_ip, (void *) EEPROM_STATIC_GATEWAY, 4);
    eeprom_write_block(_dns_ip, (void *) EEPROM_STATIC_DNS, 4);
    recomputeAndStoreConfigChecksum();
  }
}

void use_command(char * arg) {
  const uint8_t noip[4] = {0};
  if (strncmp("dhcp", arg, 3) == 0) {
    //  To switch back to using DHCP, call the setDHCP() function 
    //  Like setStaticIp, only needs to be called once.
    if (!cc3000.setDHCP()){
      Serial.println(F("Error: setDCHP Failed on CC3000"));
      return;      
    }
    else{
      eeprom_write_block(noip, (void *) EEPROM_STATIC_IP_ADDRESS, 4);
      recomputeAndStoreConfigChecksum();
    }
  }
  else {
    Serial.print(F("Error: Invalid parameter provided to 'use' command - \""));
    Serial.print(arg);
    Serial.println("\"");
    return;
  }
}

void set_mqtt_password(char * arg) {
  // we've reserved 32-bytes of EEPROM for a MQTT password
  // so the argument's length must be <= 31
  char password[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(password, arg, len);
    eeprom_write_block(password, (void *) EEPROM_MQTT_PASSWORD, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: MQTT password must be less than 32 characters in length"));
  }
}

void set_mqtt_server(char * arg){
  // we've reserved 32-bytes of EEPROM for an MQTT server name
  // so the argument's length must be <= 31
  char server[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(server, arg, len);
    eeprom_write_block(server, (void *) EEPROM_MQTT_SERVER_NAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: MQTT server name must be less than 32 characters in length"));
  }  
}

void set_mqtt_username(char * arg){
  // we've reserved 32-bytes of EEPROM for an MQTT username
  // so the argument's length must be <= 31
  char username[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(username, arg, len);
    eeprom_write_block(username, (void *) EEPROM_MQTT_USERNAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: MQTT username must be less than 32 characters in length"));
  }    
}
void set_mqtt_client_id(char * arg){
  // we've reserved 32-bytes of EEPROM for an MQTT client ID
  // but in fact an MQTT client ID must be between 1 and 23 characters
  // and must start with an letter
  char client_id[32] = {0};  
  
  if(!isalpha(arg[0])){
    Serial.println(F("Error: MQTT client ID must begin with a letter"));
    return;
  }
  
  uint16_t len = strlen(arg);
  if ((len >= 1) && (len <= 23)) {
    strncpy(client_id, arg, len);
    eeprom_write_block(client_id, (void *) EEPROM_MQTT_CLIENT_ID, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: MQTT client ID must be less between 1 and 23 characters in length"));
  }
}

void set_mqtt_authentication(char * arg) {    
  if (strncmp("enable", arg, 6) == 0) { 
    eeprom_write_byte((uint8_t *) EEPROM_MQTT_AUTH, 1);
    recomputeAndStoreConfigChecksum();
  }
  else if(strncmp("disable", arg, 7) == 0) { 
    eeprom_write_byte((uint8_t *) EEPROM_MQTT_AUTH, 0);
    recomputeAndStoreConfigChecksum();    
  }
  else {
    Serial.print(F("Error: Invalid parameter provided to 'mqttauth' command - \""));
    Serial.print(arg);
    Serial.println("\", must be either \"enable\" or \"disable\"");
    return;
  }
}

void set_mqtt_port(char * arg) {      
  uint16_t len = strlen(arg);
  boolean valid = true;
  
  for(uint16_t ii = 0; ii < len; ii++){
    if(!isdigit(arg[ii])){
      valid = false;
    } 
  }
  
  uint32_t port = 0xFFFFFFFF;
  if(valid){
    port = (uint32_t) strtoul(arg, NULL, 10);
  }
  
  if(valid && (port < 0x10000) && (port > 0)){
    eeprom_write_dword((uint32_t *) EEPROM_MQTT_PORT, port);
    recomputeAndStoreConfigChecksum();        
  }
  else {
    Serial.print(F("Error: Invalid parameter provided to 'mqttport' command - \""));
    Serial.print(arg);
    Serial.println("\", must a number between 1 and 65535 inclusive");
    return;
  }
}

void set_update_server_name(char * arg){
  // we've reserved 32-bytes of EEPROM for an update server name
  // so the argument's length must be <= 31
  char server[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(server, arg, len);
    eeprom_write_block(server, (void *) EEPROM_UPDATE_SERVER_NAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: Update server name must be less than 32 characters in length"));
  }
}

void backup(char * arg) {
  boolean valid = true;
  char tmp[32] = {0};
  uint16_t backup_check = eeprom_read_word((const uint16_t *) EEPROM_BACKUP_CHECK);

  if (strncmp("mac", arg, 3) == 0) {
    configInject("init mac\r"); // make sure the CC3000 mac address is in EEPROM
    Serial.println();
    eeprom_read_block(tmp, (const void *) EEPROM_MAC_ADDRESS, 6);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_MAC_ADDRESS, 6);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_MAC_ADDRESS_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_MAC_ADDRESS_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("mqttpwd", arg, 7) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_MQTT_PASSWORD, 32);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_MQTT_PASSWORD, 32);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_MQTT_PASSSWORD_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_MQTT_PASSSWORD_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("key", arg, 3) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_PRIVATE_KEY, 32);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_PRIVATE_KEY, 32);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_PRIVATE_KEY_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_PRIVATE_KEY_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("no2", arg, 3) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_NO2_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_NO2_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_NO2_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_NO2_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_NO2_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_NO2_CAL_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_NO2_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_NO2_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("co", arg, 2) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_CO_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_CO_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_CO_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_CO_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_CO_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_CO_CAL_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_CO_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_CO_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("all", arg, 3) == 0) {
    valid = false;
    configInject("backup mqttpwd\r");
    configInject("backup key\r");
    configInject("backup no2\r");
    configInject("backup co\r");
    configInject("backup mac\r");
    Serial.println();
  }
  else {
    valid = false;
    Serial.print(F("Error: Invalid parameter provided to 'backup' command - \""));
    Serial.print(arg);
    Serial.println("\"");
  }

  if (valid) {
    recomputeAndStoreConfigChecksum();
  }
}

boolean convertStringToFloat(char * str_to_convert, float * target) {
  char * end_ptr;
  *target = strtod(str_to_convert, &end_ptr);
  if (end_ptr != (str_to_convert + strlen(str_to_convert))) {
    return false;
  }
  return true;
}

void set_float_param(char * arg, float * eeprom_address, float (*conversion)(float)) {
  float value = 0.0;
  if (convertStringToFloat(arg, &value)) {
    if (conversion) {
      value = conversion(value);
    }
    eeprom_write_float(eeprom_address, value);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.print(F("Error: Failed to convert string \""));
    Serial.print(arg);
    Serial.println(F("\" to decimal number."));
  }
}

// convert from nA/ppm to ppb/V
// from SPEC Sensors, Sensor Development Kit, User Manual, Rev. 1.5
// M[V/ppb] = Sensitivity[nA/ppm] * TIA_Gain[kV/A] * 10^-9[A/nA] * 10^3[V/kV] * 10^-3[ppb/ppm]
// TIA_Gain[kV/A] for NO2 = 499
// slope = 1/M
float convert_no2_sensitivity_to_slope(float sensitivity) {
  float ret = 1.0e9;
  ret /= sensitivity;
  ret /= 499.0;
  return ret;
}

// sets both sensitivity and slope
void set_no2_sensitivity(char * arg) {
  set_float_param(arg, (float *) EEPROM_NO2_SENSITIVITY, 0);
  set_float_param(arg, (float *) EEPROM_NO2_CAL_SLOPE, convert_no2_sensitivity_to_slope);
}

void set_no2_slope(char * arg) {
  set_float_param(arg, (float *) EEPROM_NO2_CAL_SLOPE, 0);
}

void set_no2_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_NO2_CAL_OFFSET, 0);
}

// convert from nA/ppm to ppb/V
// from SPEC Sensors, Sensor Development Kit, User Manual, Rev. 1.5
// M[V/ppm] = Sensitivity[nA/ppm] * TIA_Gain[kV/A] * 10^-9[A/nA] * 10^3[V/kV]
// TIA_Gain[kV/A] for CO = 100
// slope = 1/M
float convert_co_sensitivity_to_slope(float sensitivity) {
  float ret = 1.0e6;
  ret /= sensitivity;
  ret /= 100.0;
  return ret;
}

void set_co_sensitivity(char * arg) {
  set_float_param(arg, (float *) EEPROM_CO_SENSITIVITY, 0);
  set_float_param(arg, (float *) EEPROM_CO_CAL_SLOPE, convert_co_sensitivity_to_slope);
}

void set_co_slope(char * arg) {
  set_float_param(arg, (float *) EEPROM_CO_CAL_SLOPE, 0);
}

void set_co_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_CO_CAL_OFFSET, 0);
}

void set_private_key(char * arg) {
  // we've reserved 32-bytes of EEPROM for a private key
  // only exact 64-character hex representation is accepted
  uint8_t key[32] = {0};
  uint16_t len = strlen(arg);
  if (len == 64) {
    // process the characters as pairs
    for (uint8_t ii = 0; ii < 32; ii++) {
      char tmp[3] = {0};
      tmp[0] = arg[ii * 2];
      tmp[1] = arg[ii * 2 + 1];
      if (isxdigit(tmp[0]) && isxdigit(tmp[1])) {
        key[ii] = (uint8_t) strtoul(tmp, NULL, 16);
      }
      else {
        Serial.print(F("Error: Invalid hex value found ["));
        Serial.print(tmp);
        Serial.println(F("]"));
        return;
      }
    }

    eeprom_write_block(key, (void *) EEPROM_PRIVATE_KEY, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: Private key must be exactly characters long, "));
    Serial.print(F("       but was "));
    Serial.print(len);
    Serial.println(F(" characters long."));
  }
}

void recomputeAndStoreConfigChecksum(void) {
  uint16_t crc = computeConfigChecksum();
  eeprom_write_word((uint16_t *) EEPROM_CRC_CHECKSUM, crc);
}

uint16_t computeConfigChecksum(void) {
  uint16_t crc = 0;
  // the checksum is 2 bytes, so start computing the checksum at
  // the second byte after it's location
  for (uint16_t address = EEPROM_CRC_CHECKSUM + 2; address <= E2END; address++) {
    crc = _crc16_update(crc, eeprom_read_byte((const uint8_t *) address));
  }
  return crc;
}

/****** GAS SENSOR SUPPORT FUNCTIONS ******/

void selectNoSlot(void) {
  digitalWrite(9, LOW);
  digitalWrite(10, LOW);
}

void selectSlot1(void) {
  selectNoSlot();
  digitalWrite(10, HIGH);
}

void selectSlot2(void) {
  selectNoSlot();
  digitalWrite(9, HIGH);
}

/****** LCD SUPPORT FUNCTIONS ******/

void backlightOn(void) {
  digitalWrite(A6, HIGH);
}

void backlightOff(void) {
  digitalWrite(A6, LOW);
}

void lcdFrownie(uint8_t pos_x, uint8_t pos_y){
  lcd.setCursor(pos_x, pos_y);
  lcd.write((byte) 1); 
}

void lcdSmiley(uint8_t pos_x, uint8_t pos_y){
  lcd.setCursor(pos_x, pos_y);
  lcd.write((byte) 0); 
}

void setLCD_P(const char str[] PROGMEM){  
  char tmp[33] = {0};
  strncpy_P(tmp, str, 32);
  setLCD(tmp);
}

void setLCD(const char str[]){
  char tmp[17] = {0};  
  uint16_t original_length = strlen(str);
  strncpy(tmp, str, 16);
  uint16_t len = strlen(tmp);
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(tmp);

  if(original_length > 16){   
    memset(tmp, 0, 16);
    strncpy(tmp, str + 16, 16);
    lcd.setCursor(0,1);  
    lcd.print(tmp);     
  }   
}

void updateLCD(const char str[], uint8_t pos_x, uint8_t pos_y, uint8_t num_chars){
  char tmp[17] = {0};
  strncpy(tmp, str, 16);
  if(num_chars < 16){
    tmp[num_chars] = '\0'; 
  }    
  
  if((pos_x < 16) && (pos_y < 2)){
    lcd.setCursor(pos_x, pos_y);
    lcd.print(tmp);    
  }
}

void updateLCD(float value, uint8_t pos_x, uint8_t pos_y, uint8_t field_width){
  char tmp[17] = {0};
  boolean requires_minus_sign = false;
  boolean requires_leading_zero = false; 
  boolean requires_decimal_point = false;
  
  uint8_t available_field_width_remaining_for_digits = field_width;
  uint8_t num_digits_before_the_decimal_point = 0;
  uint8_t num_digits_after_the_decimal_point = 0;
  // we only have field_width digits available
  // one of those *may* be a '.'
  // one of those *may* be a '-'
  
  if(value < 0.0f){
    requires_minus_sign = true;
    value *= -1.0f;   
    if(available_field_width_remaining_for_digits > 0){
      available_field_width_remaining_for_digits -= 1; // because requires_minus_sign  
    }
  }  
  
  // deal with the value as though it's positive from here forward    
  
  if(value < 1.0f){
    requires_leading_zero = true; 
    requires_decimal_point = true;
    if(available_field_width_remaining_for_digits > 1){
      available_field_width_remaining_for_digits -= 2; // because requires_leading_zero and requires_decimal_point
    }
  }
  
  // whether it requires a decimal point otherwise we need to 
  // determine based on the available space remaining for digits  
  // and the number of digits the number 'naturally' has before
  // the decimal point
  if(!requires_decimal_point){
    float x = value;    
    do {
      x /= 10.0f; 
      num_digits_before_the_decimal_point++;
    } 
    while(x >= 1.0f);
    
    // if the number of digits that must be displayed before the decimal point
    // leaves room for any additional digits, then the displaying of it requires a decimal point
    if(num_digits_before_the_decimal_point + 1 < available_field_width_remaining_for_digits){
      requires_decimal_point = true;
      if(available_field_width_remaining_for_digits > 0){
        available_field_width_remaining_for_digits -= 1; // because requires_decimal_point
      }      
    }
  }
  
  // it may be implausible to display the number in the field width at all at this point
  // if it can't be displayed, show '*' for all the entire field_width
  if(available_field_width_remaining_for_digits == 0){
    for(uint8_t ii = 0; (ii < field_width) && (ii < 16); ii++){
      tmp[ii] = '*';       
    }
  }
  else{  
    // if it requires a decimal point, determine the num digits after the decimal point to display
    // based on the num_digits_before_the_decimal_point and the field_width
    int32_t integer_part = (int32_t) value; 
    int32_t decimal_part = 0;
    if(requires_decimal_point){
      num_digits_after_the_decimal_point = available_field_width_remaining_for_digits - num_digits_before_the_decimal_point;
      
      // compute the whole and fractional parts of the number  
      // remember it's guaranteed to be positive here
      float fractional_part = value - (1.0f * integer_part);  
      for(uint8_t ii = 0; ii < num_digits_after_the_decimal_point; ii++){
        fractional_part *= 10.0f;
      }      
      fractional_part += 0.5f; // round to nearest
      decimal_part = (int32_t) fractional_part;                 
    }   

    // now that we have the fractional part and the decimal part
    // lets finally display the thing    
    if(requires_decimal_point){
      // if it's got a decimal part to display, it is guaranteed to fill
      // the entire field
      snprintf(tmp, 16, "%ld.%ld", integer_part, decimal_part);
    }
    else{
      // if it only has an integer part, it may need to be padded
      // to the field width
      char fmt_string[17] = {0};
      integer_part = (int32_t) (value + 0.5f); 
      snprintf(fmt_string, 16, "%%%dld", field_width); 
      // this amounts to something like "%3ld", where 3 is the field_width
      snprintf(tmp, 16, fmt_string, integer_part);      
    }    
  }
    
  updateLCD(tmp, pos_x, pos_y, field_width);  
}

void updateLCD(uint32_t ip, uint8_t line_number){
  char tmp[17] = {0};
  snprintf(tmp, 16, "%d.%d.%d.%d", 
    (uint8_t)(ip >> 24),
    (uint8_t)(ip >> 16),
    (uint8_t)(ip >> 8),       
    (uint8_t)(ip >> 0));    
  
  updateLCD(tmp, 1);
}

void updateLCD(const char str[], uint8_t line_number){
  // center the string on the line
  char tmp[17] = {0};  
  uint16_t original_len = strlen(str);
  if(original_len < 16){
    uint8_t num_empty_chars_on_line = 16 - original_len;
    // pad the front of the string with spaces
    uint8_t half_num_empty_chars_on_line = num_empty_chars_on_line / 2;
    for(uint8_t ii = 0; ii < half_num_empty_chars_on_line; ii++){
      tmp[ii] = ' '; 
    }    
  }
  uint16_t len = strlen(tmp);  // length of the front padding    
  if((original_len + len) <= 16){
    strcat(tmp, str); // concatenate the string into the front padding-
  }
  
  len = strlen(tmp);
  if(len < 16){
    // pad the tail of the string with spaces
    uint8_t num_trailing_spaces = 16 - len;
    for(uint8_t ii = 0; ii < num_trailing_spaces; ii++){
      tmp[len + ii] = ' ';
    }
  }
  
  if(line_number < 2){
    updateLCD(tmp, 0, line_number, 16);
  }
}

void updateLCD(int32_t value, uint8_t pos_x, uint8_t pos_y, uint8_t num_chars){
  char tmp[17] = {0};
  snprintf(tmp, num_chars, "%ld", value);
  updateLCD(tmp, pos_x, pos_y, num_chars);
}

void updateLCD(char value, uint8_t pos_x, uint8_t pos_y, uint8_t num_chars){
  char tmp[17] = {0};
  tmp[0] = value;
  updateLCD(tmp, pos_x, pos_y, num_chars);
}

void updateCornerDot(void){
  static uint8_t on = 0;
  on = 1 - on;
  if(on == 1){
    updateLCD('.', 15, 1, 1);
  } 
  else{
    updateLCD(' ', 15, 1, 1); 
  }
}

void updateLcdProgressDots(void){
  static uint8_t cnt = 0;
  cnt++;
  uint8_t num_dots = cnt % 4;
  switch(num_dots){
    case 0: 
      updateLCD("   ", 1); 
      break;
    case 1:
      updateLCD(".  ", 1); 
      break;
    case 2:
      updateLCD(".. ", 1); 
      break;
    case 3:
      updateLCD("...", 1); 
      break;          
  } 
}

/****** WIFI SUPPORT FUNCTIONS ******/
boolean restartWifi(){
  boolean first_time = true;
  
  if(!connectedToNetwork()){
    
    if(first_time){
      first_time = false;
    }
    else{
      Serial.print(F("Info: Rebooting CC3000..."));
      cc3000.reboot();
      Serial.println(F("OK."));
    }
    
    petWatchdog();
    reconnectToAccessPoint();
    petWatchdog();    
    acquireIpAddress();    
    petWatchdog();    
    displayConnectionDetails();

    // if (!mdns.begin("airqualityegg", cc3000)) {
    //   Serial.println(F("Error setting up MDNS responder!"));
    //   while(1);     
    // }    
  }
  
  return connectedToNetwork();
}

bool displayConnectionDetails(void){
  uint32_t ipAddress, netmask, gateway, dhcpserv, dnsserv;
  
  if(!cc3000.getIPAddress(&ipAddress, &netmask, &gateway, &dhcpserv, &dnsserv))
  {
    Serial.println(F("Error: Unable to retrieve the IP Address!"));
    return false;
  }
  else
  {
    Serial.print(F("Info: IP Addr: ")); cc3000.printIPdotsRev(ipAddress); Serial.println();
    Serial.print(F("Info: Netmask: ")); cc3000.printIPdotsRev(netmask); Serial.println();
    Serial.print(F("Info: Gateway: ")); cc3000.printIPdotsRev(gateway); Serial.println();
    Serial.print(F("Info: DHCPsrv: ")); cc3000.printIPdotsRev(dhcpserv); Serial.println();
    Serial.print(F("Info: DNSserv: ")); cc3000.printIPdotsRev(dnsserv); Serial.println();
    
    updateLCD(ipAddress, 1);
    lcdSmiley(15, 1); // lower right corner
    delay(LCD_SUCCESS_MESSAGE_DELAY);  
  
    return true;
  }
}

void reconnectToAccessPoint(void){
  char ssid[32] = {0};
  char network_password[32] = {0};
  
  uint8_t connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);
  uint8_t network_security_mode = eeprom_read_byte((const uint8_t *) EEPROM_SECURITY_MODE);  
  eeprom_read_block(ssid, (const void *) EEPROM_SSID, 31);
  eeprom_read_block(network_password, (const void *) EEPROM_NETWORK_PWD, 31); 
 
  switch(connect_method){
    case CONNECT_METHOD_DIRECT:
      Serial.print(F("Info: Connecting to Access Point with SSID \""));
      Serial.print(ssid);
      Serial.print(F("\"..."));
      setLCD_P(PSTR("CONNECTING TO AP"));
      updateLCD(ssid, 1);
      
      if(!cc3000.connectToAP(ssid, network_password, network_security_mode)) {
        Serial.print(F("Error: Failed to connect to Access Point with SSID: "));
        Serial.println(ssid);
        Serial.flush();
        updateLCD("FAILED", 1);
        lcdFrownie(15, 1);
        delay(LCD_ERROR_MESSAGE_DELAY);
        watchdogForceReset();
      }
      Serial.println(F("OK."));
      updateLCD("CONNECTED", 1);
      lcdSmiley(15, 1);
      delay(LCD_SUCCESS_MESSAGE_DELAY);
      break;
    case CONNECT_METHOD_SMARTCONFIG:
    case CONNECT_METHOD_PFOD:
    default:
      Serial.println(F("Error: Connection method not currently supported"));
      break;
  }  
}

void acquireIpAddress(void){
  uint8_t static_ip_address[4] = {0};
  uint8_t noip[4] = {0};
  eeprom_read_block(static_ip_address, (const void *) EEPROM_STATIC_IP_ADDRESS, 4);
  
  // if it's DHCP we're configured for, engage DHCP process
  if (memcmp(static_ip_address, noip, 4) == 0){
    /* Wait for DHCP to complete */
    Serial.print(F("Info: Request DHCP..."));
    setLCD_P(PSTR(" REQUESTING IP  "));   
    
    while (!cc3000.checkDHCP()){      
      // if this goes on for longer than a minute, 
      // tiny watchdog should automatically kick in
      // and reset the unit. If we don't want that to happen
      // we would need to pet the tiny watchdog every so often
      // in this loop.
      
      current_millis = millis();
      if(current_millis - previous_touch_sampling_millis >= touch_sampling_interval){
        previous_touch_sampling_millis = current_millis;    
        collectTouch();    
        processTouchQuietly();  
      }      

      if(current_millis - previous_progress_dots_millis >= progress_dots_interval){
        previous_progress_dots_millis = current_millis;
        updateLcdProgressDots();
      }     
     
     delay(100); 
      
    }   
    Serial.println(F("OK.")); 
  }
}

boolean connectedToNetwork(void){
  return (cc3000.getStatus() == STATUS_CONNECTED);
}

void cc3000IpToArray(uint32_t ip, uint8_t * ip_array){
  for(uint8_t ii = 0; ii < 4; ii++){
    ip_array[ii] = ip & 0xff;
    ip >>= 8;
  }
}

/****** ADC SUPPORT FUNCTIONS ******/
// returns the measured voltage in Volts
// 62.5 microvolts resolution in 16-bit mode
boolean burstSampleADC(float * result){
  #define NUM_SAMPLES_PER_BURST (8)
  MCP342x::Config status;
  int32_t burst_sample_total = 0;
  uint8_t num_samples = 0;
  int32_t value = 0;
  for(uint8_t ii = 0; ii < NUM_SAMPLES_PER_BURST; ii++){    
    uint8_t err = adc.convertAndRead(MCP342x::channel1, MCP342x::oneShot, 
      MCP342x::resolution16, MCP342x::gain1, 75000, value, status);          
    if(err == 0){
      burst_sample_total += value;
      num_samples++;
    }
    else{  
      Serial.print(F("Error: ADC Read Error ["));    
      Serial.print((uint32_t) err, HEX); 
      Serial.println(F("]"));
    }
  }
  
  if(num_samples > 0){
    *result = (62.5e-6f * burst_sample_total) / num_samples;  
    return true;
  }

  return false;
}

/****** MQTT SUPPORT FUNCTIONS ******/
boolean mqttResolve(void){
  uint32_t ip = 0;
  static boolean resolved = false;
  
  char mqtt_server_name[32] = {0};
  if(!resolved){
    eeprom_read_block(mqtt_server_name, (const void *) EEPROM_MQTT_SERVER_NAME, 31);
    setLCD_P(PSTR("   RESOLVING"));
    updateLCD(mqtt_server_name, 1);
    if  (!cc3000.getHostByName(mqtt_server_name, &ip) || (ip == 0))  {
      Serial.print(F("Error: Couldn't resolve '"));
      Serial.print(mqtt_server_name);
      Serial.println(F("'"));
      
      updateLCD("FAILED", 1);
      lcdFrownie(15, 1);
      delay(LCD_ERROR_MESSAGE_DELAY);
      return false;
    }  
    else{
      resolved = true;
      cc3000IpToArray(ip, mqtt_server);      
      Serial.print(F("Info: Resolved \""));
      Serial.print(mqtt_server_name);
      Serial.print(F("\" to IP address "));
      cc3000.printIPdotsRev(ip);
      
      updateLCD(ip, 1);      
      lcdSmiley(15, 1);
      delay(LCD_SUCCESS_MESSAGE_DELAY);          
      Serial.println();    
    }
  }
    
  return true;
}

boolean mqttReconnect(void){
   static boolean first_access = true;
   static char mqtt_username[32] = {0};
   static char mqtt_password[32] = {0};
   static uint8_t mqtt_auth_enabled = 0;
   static uint32_t mqtt_port = 0;
   
   boolean loop_return_flag = true;
   
   if(!mqttResolve()){
     return false;
   }
     
   if(first_access){
     first_access = false;
     loop_return_flag = false;
     eeprom_read_block(mqtt_username, (const void *) EEPROM_MQTT_USERNAME, 31);
     eeprom_read_block(mqtt_client_id, (const void *) EEPROM_MQTT_CLIENT_ID, 31);
     eeprom_read_block(mqtt_password, (const void *) EEPROM_MQTT_PASSWORD, 31);
     mqtt_auth_enabled = eeprom_read_byte((const uint8_t *) EEPROM_MQTT_AUTH);
     mqtt_port = eeprom_read_dword((const uint32_t *) EEPROM_MQTT_PORT);

     mqtt_client.setBrokerIP(mqtt_server);
     mqtt_client.setPort(mqtt_port);
     mqtt_client.setClient(wifiClient);
          
   }
   else{
     loop_return_flag = mqtt_client.loop();
   }           
   
   if(!loop_return_flag){
     Serial.print(F("Info: Connecting to MQTT Broker with Client ID \""));
     Serial.print(mqtt_client_id);
     Serial.print(F("\" "));
     boolean connect_status = false;
     if(mqtt_auth_enabled){
       Serial.print(F("using Authentication..."));
       connect_status = mqtt_client.connect(mqtt_client_id, mqtt_username, mqtt_password);
     }
     else{
       Serial.print(F("Without Authentication..."));
       connect_status = mqtt_client.connect(mqtt_client_id);
     }
     
     if (connect_status) {
       Serial.println(F("OK."));
       return true;
     }
     else{
       Serial.println(F("Failed.")); 
       return false;       
     }    
   }  
}

boolean mqqtPublish(char * topic, char *str){
  boolean response_status = true;
  Serial.print(F("MQTT publishing to topic "));
  Serial.print(topic);
  Serial.print(F("..."));
  if(mqtt_client.publish(topic, str)){
    Serial.println(F("OK."));
    response_status = true;
  } 
  else {
    Serial.println(F("Failed."));
    response_status = false;
  }
  
  return response_status;
}


boolean publishHeartbeat(){
  static uint32_t post_counter = 0;
  char tmp[128] = { 0 };  
  uint8_t sample = pgm_read_byte(&heartbeat_waveform[heartbeat_waveform_index++]);
  snprintf(tmp, 127, "{\"converted-value\" : %d, \"firmware-version\": \"%s\", \"counter\" : %lu}", sample, firmware_version, post_counter++);  
  if(heartbeat_waveform_index >= NUM_HEARTBEAT_WAVEFORM_SAMPLES){
     heartbeat_waveform_index = 0;
  }
  
  return mqqtPublish("/orgs/wd/aqe/heartbeat", tmp); 
}

boolean publishTemperature(){
  char tmp[128] = { 0 };  
  char value_string[16] = {0};
  float temperature_moving_average = calculateAverage(temperature_sample_buffer, TEMPERATURE_SAMPLE_BUFFER_DEPTH);
  temperature_degc = temperature_moving_average;
  dtostrf(temperature_moving_average, -6, 2, value_string);
  snprintf(tmp, 127, "{\"converted-value\" : %s, \"converted-units\": \"degC\"}", value_string);    
  return mqqtPublish("/orgs/wd/aqe/temperature", tmp);   
}

boolean publishHumidity(){
  char tmp[128] = { 0 };  
  char value_string[16] = {0};  
  float humidity_moving_average = calculateAverage(humidity_sample_buffer, HUMIDITY_SAMPLE_BUFFER_DEPTH);
  relative_humidity_percent = humidity_moving_average;
  dtostrf(humidity_moving_average, -6, 2, value_string);
  snprintf(tmp, 127, "{\"converted-value\" : %s, \"converted-units\": \"percent\"}", value_string);  
  return mqqtPublish("/orgs/wd/aqe/humidity", tmp); 
}

void collectTemperature(void){
  static uint8_t sample_write_index = 0;
  float raw_value = 0.0f;
  if(init_sht25_ok){
    if(sht25.getTemperature(&raw_value)){
      temperature_sample_buffer[sample_write_index++] = raw_value;
      
      if(sample_write_index == TEMPERATURE_SAMPLE_BUFFER_DEPTH){
        sample_write_index = 0; 
        temperature_ready = true;
      }
    }
  }
}

void collectHumidity(void){
  static uint8_t sample_write_index = 0;
  float raw_value = 0.0f;
  if(init_sht25_ok){
    if(sht25.getRelativeHumidity(&raw_value)){
      humidity_sample_buffer[sample_write_index++] = raw_value;
      
      if(sample_write_index == HUMIDITY_SAMPLE_BUFFER_DEPTH){
        sample_write_index = 0; 
        humidity_ready = true;
      }
    }
  }
}

void collectTouch(void){
  static uint8_t sample_write_index = 0;
  touch_sample_buffer[sample_write_index++] = touch.capacitiveSensor(30);
  
  if(sample_write_index == TOUCH_SAMPLE_BUFFER_DEPTH){
    sample_write_index = 0; 
  }
}

void processTouchVerbose(boolean verbose_output){
  const uint32_t touch_event_threshold = 85UL;  
  static unsigned long touch_start_millis = 0UL;
  const long backlight_interval = 10000L; 
  static boolean backlight_is_on = false;;
  
  float touch_moving_average = calculateAverage(touch_sample_buffer, TOUCH_SAMPLE_BUFFER_DEPTH); 
  if(verbose_output){
    Serial.print(F("Info: Average Touch Reading: "));
    Serial.println(touch_moving_average);
  }
  
  if(touch_moving_average > touch_event_threshold){
    backlightOn();
    backlight_is_on = true;
    if(verbose_output){
      Serial.println(F("Info: Turning backlight on."));
    }
    touch_start_millis = current_millis;
  } 
  
  if((current_millis - touch_start_millis) >= backlight_interval) {        
    if(backlight_is_on){      
      if(verbose_output){
        Serial.println(F("Info: Turning backlight off (timer expired)."));   
      }
      backlightOff();      
      backlight_is_on = false;      
    }
  }  
}

void processTouch(void){
  processTouchVerbose(true);
}

void processTouchQuietly(void){
  processTouchVerbose(false);
}

void collectNO2(void ){
  static uint8_t sample_write_index = 0;
  float raw_value = 0.0f;
  
  if(init_no2_afe_ok && init_no2_adc_ok){
    selectSlot2();  
    if(burstSampleADC(&raw_value)){      
      no2_sample_buffer[sample_write_index++] = raw_value;
      
      if(sample_write_index == NO2_SAMPLE_BUFFER_DEPTH){
        sample_write_index = 0; 
        no2_ready = true;
      }
    }
  }
    
  selectNoSlot(); 
}

void collectCO(void ){
  static uint8_t sample_write_index = 0;
  float raw_value = 0.0f;
  
  if(init_co_afe_ok && init_co_adc_ok){
    selectSlot1();  
    if(burstSampleADC(&raw_value)){   
      co_sample_buffer[sample_write_index++] = raw_value;
      
      if(sample_write_index == CO_SAMPLE_BUFFER_DEPTH){
        sample_write_index = 0; 
        co_ready = true;
      }      
    }
  }
    
  selectNoSlot();     
}

void no2_convert_from_volts_to_ppb(float volts, float * converted_value, float * temperature_compensated_value){
  static boolean first_access = true;
  static float no2_zero_volts = 0.0f;
  static float no2_slope_ppb_per_volt = 0.0f;
  float temperature_coefficient_of_span = 0.0f;
  float temperature_compensated_slope = 0.0f;
  if(first_access){
    // NO2 has negative slope in circuit, more negative voltages correspond to higher levels of NO2
    no2_slope_ppb_per_volt = -1.0 * eeprom_read_float((const float *) EEPROM_NO2_CAL_SLOPE); 
    no2_zero_volts = eeprom_read_float((const float *) EEPROM_NO2_CAL_OFFSET);
    first_access = false;
  }
  
  // piecewise temperature coefficient of span for NO2
  // -20C to 20C  0.1%/degC 
  // 20C to 50C   0.4%/degC
  if(temperature_degc < 20.0f){
    temperature_coefficient_of_span = 0.001f; // 0.1%
  }
  else{
    temperature_coefficient_of_span = 0.004f; // 0.4%    
  }
  
  temperature_compensated_slope = no2_slope_ppb_per_volt;
  temperature_compensated_slope /= (1.0f + (temperature_coefficient_of_span * (20 - temperature_degc)));
  
  *converted_value = (volts - no2_zero_volts) * no2_slope_ppb_per_volt;
  if(*converted_value <= 0.0f){
    *converted_value = 0.0f; 
  }
  
  *temperature_compensated_value = (volts - no2_zero_volts) * temperature_compensated_slope;
  if(*temperature_compensated_value <= 0.0f){
    *temperature_compensated_value = 0.0f;
  }
}

float calculateAverage(float * buf, uint8_t num_samples){
  float average = 0.0f;
  for(uint8_t ii = 0; ii < num_samples; ii++){
    average += buf[ii];
  } 
  
  return average / num_samples;
}

boolean publishNO2(){
  char tmp[512] = { 0 };  
  char raw_value_string[16] = {0};  
  char converted_value_string[16] = {0};
  char compensated_value_string[16] = {0};
  float converted_value = 0.0f, compensated_value = 0.0f;    
  float no2_moving_average = calculateAverage(no2_sample_buffer, NO2_SAMPLE_BUFFER_DEPTH);
  no2_convert_from_volts_to_ppb(no2_moving_average, &converted_value, &compensated_value);
  no2_ppb = compensated_value;  
  dtostrf(no2_moving_average, -8, 5, raw_value_string);
  dtostrf(converted_value, -4, 2, converted_value_string);
  dtostrf(compensated_value, -4, 2, compensated_value_string);    
  snprintf(tmp, 511, "{\"raw-value\" : %s, "
    "\"raw-units\": \"volt\", "
    "\"converted-value\" : %s, "
    "\"converted-units\": \"ppb\", "
    "\"compensated-value\": %s}", 
    raw_value_string, 
    converted_value_string, 
    compensated_value_string);  
  return mqqtPublish("/orgs/wd/aqe/no2", tmp);     
}

void co_convert_from_volts_to_ppm(float volts, float * converted_value, float * temperature_compensated_value){
  static boolean first_access = true;
  static float co_zero_volts = 0.0f;
  static float co_slope_ppm_per_volt = 0.0f;
  float temperature_coefficient_of_span = 0.0f;
  float temperature_compensated_slope = 0.0f;  
  if(first_access){
    // CO has positive slope in circuit, more positive voltages correspond to higher levels of CO
    co_slope_ppm_per_volt = eeprom_read_float((const float *) EEPROM_CO_CAL_SLOPE);
    co_zero_volts = eeprom_read_float((const float *) EEPROM_CO_CAL_OFFSET);
    first_access = false;
  }

  // piecewise temperature coefficient of span for NO2
  // -20C to 20C  0.6%/degC
  // 20C to 50C   0.4%/degC
  if(temperature_degc < 20.0f){
    temperature_coefficient_of_span = 0.006f; // 0.1%
  }
  else{
    temperature_coefficient_of_span = 0.004f; // 0.4%    
  }
  
  temperature_compensated_slope = co_slope_ppm_per_volt;
  temperature_compensated_slope /= (1.0f + (temperature_coefficient_of_span * (20 - temperature_degc)));
  
  
  *converted_value = (volts - co_zero_volts) * co_slope_ppm_per_volt;
  if(*converted_value <= 0.0f){
    *converted_value = 0.0f; 
  }
  
  *temperature_compensated_value = (volts - co_zero_volts) * temperature_compensated_slope;
  if(*temperature_compensated_value <= 0.0f){
    *temperature_compensated_value = 0.0f;
  }  
}

boolean publishCO(){
  char tmp[512] = { 0 };  
  char raw_value_string[16] = {0};  
  char converted_value_string[16] = {0};
  char compensated_value_string[16] = {0};
  float converted_value = 0.0f, compensated_value = 0.0f;   
  float co_moving_average = calculateAverage(co_sample_buffer, CO_SAMPLE_BUFFER_DEPTH);
  co_convert_from_volts_to_ppm(co_moving_average, &converted_value, &compensated_value);
  co_ppm = compensated_value;  
  dtostrf(co_moving_average, -8, 5, raw_value_string);
  dtostrf(converted_value, -4, 2, converted_value_string);
  dtostrf(compensated_value, -4, 2, compensated_value_string);    
  snprintf(tmp, 511, "{\"raw-value\" : %s, "
    "\"raw-units\": \"volt\", "
    "\"converted-value\" : %s, "
    "\"converted-units\": \"ppm\", "
    "\"compensated-value\": %s}", 
    raw_value_string, 
    converted_value_string, 
    compensated_value_string);  
  return mqqtPublish("/orgs/wd/aqe/co", tmp);
}

void petWatchdog(void){
  tinywdt.pet(); 
}

void watchdogForceReset(void){
  tinywdt.force_reset(); 
}

void watchdogInitialize(void){
  tinywdt.begin(500, 60000); 
}

// modal operation loop functions
void loop_wifi_mqtt_mode(void){
  static uint8_t num_mqtt_connect_retries = 0;
  static uint8_t num_mqtt_intervals_without_wifi = 0; 

  if(current_millis - previous_mqtt_publish_millis >= mqtt_publish_interval){   
    previous_mqtt_publish_millis = current_millis;      
    
    if(connectedToNetwork()){
      num_mqtt_intervals_without_wifi = 0;
      
      if(mqttReconnect()){ 
        updateLCD("TEMP", 0, 0, 4);
        updateLCD("RH", 10, 0, 2);         
        updateLCD("NO2", 0, 1, 3);
        updateLCD("CO", 10, 1, 2);
                      
        //connected to MQTT server and connected to Wi-Fi network        
        num_mqtt_connect_retries = 0;   
        if(!publishHeartbeat()){
          Serial.println(F("Error: Failed to publish Heartbeat."));  
        }
        
        if(temperature_ready){
          if(!publishTemperature()){          
            Serial.println(F("Error: Failed to publish Temperature."));          
          }
          else{
            updateLCD(temperature_degc, 5, 0, 3);             
          }
        }
        else{
            updateLCD("---", 5, 0, 3);
        }        
        
        if(humidity_ready){
          if(!publishHumidity()){
            Serial.println(F("Error: Failed to publish Humidity."));         
          }
          else{
            updateLCD(relative_humidity_percent, 13, 0, 3);  
          }
        }
        else{
          updateLCD("---", 13, 0, 3);
        }
        
        if(no2_ready){
          if(!publishNO2()){
            Serial.println(F("Error: Failed to publish NO2."));          
          }
          else{
            updateLCD(no2_ppb, 5, 1, 3);  
          }
        }
        else{
          updateLCD("---", 5, 1, 3); 
        }
        
        if(co_ready){
          if(!publishCO()){
            Serial.println(F("Error: Failed to publish CO."));         
          }
          else{
            updateLCD(co_ppm, 13, 1, 3); 
          }
        }
        else{
          updateLCD("---", 13, 1, 3);  
        }
    
      }
      else{
        // not connected to MQTT server
        num_mqtt_connect_retries++;
        if(num_mqtt_connect_retries >= 5){
          Serial.println(F("Error: MQTT Connect Failed 5 consecutive times. Forcing reboot."));
          Serial.flush();
          watchdogForceReset();  
        }
      }
    }
    else{
      // not connected to Wi-Fi network
      num_mqtt_intervals_without_wifi++;
      if(num_mqtt_intervals_without_wifi >= 5){
        Serial.println(F("Error: WiFi Re-connect Failed 5 consecutive times. Forcing reboot."));
        Serial.flush();
        watchdogForceReset();  
      }
      
      restartWifi();
    }
  }    
}

boolean loop_zeroing_mode(void){
  boolean complete = false;
  
  if(current_millis - previous_zero_check_millis >= zero_check_interval){   
    if(no2_ready){
      updateLCD(no2_ppb, 5, 1, 3);  
    }
    else{
      updateLCD("---", 5, 1, 3); 
    }
    
    if(co_ready){
      updateLCD(co_ppm, 13, 1, 3); 
    }
    else{
      updateLCD("---", 13, 1, 3);  
    }  
  }
  
  return complete;
}

boolean mode_requires_wifi(uint8_t opmode){
  boolean requires_wifi = false;
  
  if(opmode == SUBMODE_NORMAL){
    requires_wifi = true;
  } 
  
  return requires_wifi;
}
