#include <Wire.h>
#include <SPI.h>
#include <WildFire.h>
#include <WildFire_CC3000.h>
#include <SdFat.h>
#include <RTClib.h>
#include <RTC_DS3231.h>
#include <Time.h>
#include <TinyWatchdog.h>
#include <SHT25.h>
#include <MCP342x.h>
#include <LMP91000.h>
#include <WildFire_SPIFlash.h>
#include <CapacitiveSensor.h>
#include <LiquidCrystal.h>
#include <PubSubClient.h>
#include <util/crc16.h>
#include <math.h>
#include <TinyGPS.h>

// semantic versioning - see http://semver.org/
#define AQEV2FW_MAJOR_VERSION 2
#define AQEV2FW_MINOR_VERSION 0
#define AQEV2FW_PATCH_VERSION 9

#define WLAN_SEC_AUTO (10) // made up to support auto-config of security

// the start address of the second to last 4k page, where config is backed up off MCU
// the last page is reserved for use by the bootloader
#define SECOND_TO_LAST_4K_PAGE_ADDRESS      0x7E000     

WildFire wf;
WildFire_CC3000 cc3000;
TinyWatchdog tinywdt;
LMP91000 lmp91000;
MCP342x adc;
SHT25 sht25;
WildFire_SPIFlash flash;
CapacitiveSensor touch = CapacitiveSensor(A0, A1);
LiquidCrystal lcd(A3, A2, 4, 5, 6, 8);
char g_lcd_buffer[2][17] = {0}; // 2 rows of 16 characters each, with space for NULL terminator
byte mqtt_server_ip[4] = { 0 };    
PubSubClient mqtt_client;
char mqtt_client_id[32] = {0};
WildFire_CC3000_Client wifiClient;
WildFire_CC3000_Client ntpClient;
RTC_DS3231 rtc;
SdFat SD;

TinyGPS gps;
Stream * gpsSerial = &Serial1;   // TODO: This will need to be solved if we allocate Serial1 to anything else in the future
#define GPS_MQTT_STRING_LENGTH (128)
#define GPS_CSV_STRING_LENGTH (64)
char gps_mqtt_string[GPS_MQTT_STRING_LENGTH] = {0};
char gps_csv_string[GPS_CSV_STRING_LENGTH] = {0}; 

uint32_t update_server_ip32 = 0;
char update_server_name[32] = {0};
unsigned long integrity_num_bytes_total = 0;
unsigned long integrity_crc16_checksum = 0;
uint32_t flash_file_size = 0;
uint16_t flash_signature = 0;
boolean downloaded_integrity_file = false;
boolean integrity_check_succeeded = false;
boolean allowed_to_write_config_eeprom = false;

unsigned long current_millis = 0;
char firmware_version[16] = {0};
uint8_t temperature_units = 'C';
float reported_temperature_offset_degC = 0.0f;
float reported_humidity_offset_percent = 0.0f;

float temperature_degc = 0.0f;
float relative_humidity_percent = 0.0f;
float so2_ppb = 0.0f;
float o3_ppb = 0.0f;

float gps_latitude = TinyGPS::GPS_INVALID_F_ANGLE;
float gps_longitude = TinyGPS::GPS_INVALID_F_ANGLE;
float gps_altitude = TinyGPS::GPS_INVALID_F_ALTITUDE;
unsigned long gps_age = TinyGPS::GPS_INVALID_AGE;

#define MAX_SAMPLE_BUFFER_DEPTH (240) // 20 minutes @ 5 second resolution
#define SO2_SAMPLE_BUFFER         (0)
#define O3_SAMPLE_BUFFER          (1)
#define TEMPERATURE_SAMPLE_BUFFER (2)
#define HUMIDITY_SAMPLE_BUFFER    (3)
float sample_buffer[4][MAX_SAMPLE_BUFFER_DEPTH] = {0};
uint16_t sample_buffer_idx = 0;

uint32_t sampling_interval = 0;    // how frequently the sensorss are sampled
uint16_t sample_buffer_depth = 0;  // how many samples are kept in memory for averaging
uint32_t reporting_interval = 0;   // how frequently readings are reported (to wifi or console/sd)

#define TOUCH_SAMPLE_BUFFER_DEPTH (4)
float touch_sample_buffer[TOUCH_SAMPLE_BUFFER_DEPTH] = {0};

#define LCD_ERROR_MESSAGE_DELAY   (4000)
#define LCD_SUCCESS_MESSAGE_DELAY (2000)

boolean so2_ready = false;
boolean o3_ready = false;
boolean temperature_ready = false;
boolean humidity_ready = false;

boolean init_sht25_ok = false;
boolean init_o3_afe_ok = false;
boolean init_so2_afe_ok = false;
boolean init_o3_adc_ok = false;
boolean init_so2_adc_ok = false;
boolean init_spi_flash_ok = false;
boolean init_cc3000_ok = false;
boolean init_sdcard_ok = false;
boolean init_rtc_ok = false;

#define BACKLIGHT_OFF_AT_STARTUP (0)
#define BACKLIGHT_ON_AT_STARTUP  (1)
#define BACKLIGHT_ALWAYS_ON      (2)
#define BACKLIGHT_ALWAYS_OFF     (3)

// the software's operating mode
#define MODE_CONFIG      (1)
#define MODE_OPERATIONAL (2)
// submodes of normal behavior
#define SUBMODE_NORMAL   (3)
// #define SUBMODE_ZEROING  (4) // deprecated for SUBMODE_OFFLINE
#define SUBMODE_OFFLINE  (5)

uint8_t mode = MODE_OPERATIONAL;

// the config mode state machine's return values
#define CONFIG_MODE_NOTHING_SPECIAL  (0)
#define CONFIG_MODE_GOT_INIT         (1)
#define CONFIG_MODE_GOT_EXIT         (2)

#define EEPROM_CONFIG_MEMORY_SIZE (1024)

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
#define EEPROM_SO2_SENSITIVITY    (EEPROM_MQTT_PASSWORD - 4)      // float value, 4-bytes, the sensitivity from the sticker
#define EEPROM_SO2_CAL_SLOPE      (EEPROM_SO2_SENSITIVITY - 4)    // float value, 4-bytes, the slope applied to the sensor
#define EEPROM_SO2_CAL_OFFSET     (EEPROM_SO2_CAL_SLOPE - 4)      // float value, 4-btyes, the offset applied to the sensor
#define EEPROM_O3_SENSITIVITY     (EEPROM_SO2_CAL_OFFSET - 4)     // float value, 4-bytes, the sensitivity from the sticker
#define EEPROM_O3_CAL_SLOPE       (EEPROM_O3_SENSITIVITY - 4)     // float value, 4-bytes, the slope applied to the sensor
#define EEPROM_O3_CAL_OFFSET      (EEPROM_O3_CAL_SLOPE - 4)       // float value, 4-bytes, the offset applied to the sensor
#define EEPROM_PRIVATE_KEY        (EEPROM_O3_CAL_OFFSET - 32)     // 32-bytes of Random Data (256-bits)
#define EEPROM_MQTT_SERVER_NAME   (EEPROM_PRIVATE_KEY - 32)       // string, the DNS name of the MQTT server (default opensensors.io), up to 32 characters (one of which is a null terminator)
#define EEPROM_MQTT_USERNAME      (EEPROM_MQTT_SERVER_NAME - 32)  // string, the user name for the MQTT server (default wickeddevice), up to 32 characters (one of which is a null terminator)
#define EEPROM_MQTT_CLIENT_ID     (EEPROM_MQTT_USERNAME - 32)     // string, the client identifier for the MQTT server (default SHT25 identifier), between 1 and 23 characters long
#define EEPROM_MQTT_AUTH          (EEPROM_MQTT_CLIENT_ID - 1)     // MQTT authentication enabled, single byte value 0 = disabled or 1 = enabled
#define EEPROM_MQTT_PORT          (EEPROM_MQTT_AUTH - 4)          // MQTT authentication enabled, reserve four bytes, even though you only need two for a port
#define EEPROM_UPDATE_SERVER_NAME (EEPROM_MQTT_PORT - 32)         // string, the DNS name of the Firmware Update server (default update.wickeddevice.com), up to 32 characters (one of which is a null terminator)
#define EEPROM_OPERATIONAL_MODE   (EEPROM_UPDATE_SERVER_NAME - 1) // operational mode encoded as a single byte value (e.g. NORMAL, OFFLINE, etc.)
#define EEPROM_TEMPERATURE_UNITS  (EEPROM_OPERATIONAL_MODE - 1)   // temperature units 'F' for Fahrenheit and 'C' for Celsius
#define EEPROM_UPDATE_FILENAME    (EEPROM_TEMPERATURE_UNITS - 32) // 32-bytes for the update server filename (excluding the implied extension)
#define EEPROM_TEMPERATURE_OFFSET (EEPROM_UPDATE_FILENAME - 4)    // float value, 4-bytes, the offset applied to the sensor for reporting
#define EEPROM_HUMIDITY_OFFSET    (EEPROM_TEMPERATURE_OFFSET - 4) // float value, 4-bytes, the offset applied to the sensor for reporting
#define EEPROM_BACKLIGHT_DURATION (EEPROM_HUMIDITY_OFFSET - 2)    // integer value, 2-bytes, how long, in seconds the backlight should stay on when it turns on
#define EEPROM_BACKLIGHT_STARTUP  (EEPROM_BACKLIGHT_DURATION - 1) // boolean value, whether or not the backlight should turn on at startup
#define EEPROM_SAMPLING_INTERVAL  (EEPROM_BACKLIGHT_STARTUP - 2)  // integer value, number of seconds between sensor samplings
#define EEPROM_REPORTING_INTERVAL (EEPROM_SAMPLING_INTERVAL - 2)  // integer value, number of seconds between sensor reports
#define EEPROM_AVERAGING_INTERVAL (EEPROM_REPORTING_INTERVAL - 2) // integer value, number of seconds of samples averaged
#define EEPROM_ALTITUDE_METERS    (EEPROM_AVERAGING_INTERVAL - 2) // signed integer value, 2-bytes, the altitude in meters above sea level, where the Egg is located
#define EEPROM_MQTT_TOPIC_PREFIX  (EEPROM_ALTITUDE_METERS - 64)   // up to 64-character string, prefix prepended to logical sensor topics
#define EEPROM_USE_NTP            (EEPROM_MQTT_TOPIC_PREFIX - 1)  // 1 means use NTP, anything else means don't use NTP
#define EEPROM_NTP_SERVER_NAME    (EEPROM_USE_NTP - 32)           // 32-bytes for the NTP server to use
#define EEPROM_NTP_TZ_OFFSET_HRS  (EEPROM_NTP_SERVER_NAME - 4)    // timezone offset as a floating point value
#define EEPROM_MQTT_TOPIC_SUFFIX_ENABLED  (EEPROM_NTP_TZ_OFFSET_HRS - 1)    // a simple flag to indicate whether or not the topic suffix is enabled
#define EEPROM_2_2_0_SAMPLING_UPD (EEPROM_MQTT_TOPIC_SUFFIX_ENABLED - 1)          // 1 means to sampling parameter default changes have been applied
//  /\
//   L Add values up here by subtracting offsets to previously added values
//   * ... and make sure the addresses don't collide and start overlapping!
//   T Add values down here by adding offsets to previously added values
//  \/
#define EEPROM_BACKUP_NTP_TZ_OFFSET_HRS  (EEPROM_BACKUP_HUMIDITY_OFFSET + 4)
#define EEPROM_BACKUP_HUMIDITY_OFFSET    (EEPROM_BACKUP_TEMPERATURE_OFFSET + 4)
#define EEPROM_BACKUP_TEMPERATURE_OFFSET (EEPROM_BACKUP_PRIVATE_KEY + 32)
#define EEPROM_BACKUP_PRIVATE_KEY        (EEPROM_BACKUP_O3_CAL_OFFSET + 4)
#define EEPROM_BACKUP_O3_CAL_OFFSET      (EEPROM_BACKUP_O3_CAL_SLOPE + 4)
#define EEPROM_BACKUP_O3_CAL_SLOPE       (EEPROM_BACKUP_O3_SENSITIVITY + 4)
#define EEPROM_BACKUP_O3_SENSITIVITY     (EEPROM_BACKUP_SO2_CAL_OFFSET + 4)
#define EEPROM_BACKUP_SO2_CAL_OFFSET     (EEPROM_BACKUP_SO2_CAL_SLOPE + 4)
#define EEPROM_BACKUP_SO2_CAL_SLOPE      (EEPROM_BACKUP_SO2_SENSITIVITY + 4)
#define EEPROM_BACKUP_SO2_SENSITIVITY    (EEPROM_BACKUP_MQTT_PASSWORD + 32)
#define EEPROM_BACKUP_MQTT_PASSWORD      (EEPROM_BACKUP_MAC_ADDRESS + 6)
#define EEPROM_BACKUP_MAC_ADDRESS        (EEPROM_BACKUP_CHECK + 2) // backup parameters are added here offset from the EEPROM_CRC_CHECKSUM
#define EEPROM_BACKUP_CHECK              (EEPROM_CRC_CHECKSUM + 2) // 2-byte value with various bits set if backup has ever happened
#define EEPROM_CRC_CHECKSUM              (E2END + 1 - EEPROM_CONFIG_MEMORY_SIZE) // reserve the last 1kB for config
// the only things that need "backup" are those which are unique to a device
// other things can have "defaults" stored in flash (i.e. using the restore defaults command)

// valid connection methods
// only DIRECT is supported initially
#define CONNECT_METHOD_DIRECT        (0)

// backup status bits
#define BACKUP_STATUS_MAC_ADDRESS_BIT             (7)
#define BACKUP_STATUS_MQTT_PASSSWORD_BIT          (6)
#define BACKUP_STATUS_SO2_CALIBRATION_BIT         (5)
#define BACKUP_STATUS_O3_CALIBRATION_BIT          (4)
#define BACKUP_STATUS_PRIVATE_KEY_BIT             (3)
#define BACKUP_STATUS_TEMPERATURE_CALIBRATION_BIT (2)
#define BACKUP_STATUS_HUMIDITY_CALIBRATION_BIT    (1)
#define BACKUP_STATUS_TIMEZONE_CALIBRATION_BIT    (0)

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
void set_mqtt_topic_prefix(char * arg);
void backup(char * arg);
void set_so2_slope(char * arg);
void set_so2_offset(char * arg);
void set_so2_sensitivity(char * arg);
void set_o3_slope(char * arg);
void set_o3_offset(char * arg);
void set_o3_sensitivity(char * arg);
void set_reported_temperature_offset(char * arg);
void set_reported_humidity_offset(char * arg);
void set_private_key(char * arg);
void set_operational_mode(char * arg);
void set_temperature_units(char * arg);
void set_update_filename(char * arg);
void force_command(char * arg);
void set_backlight_behavior(char * arg);
void AQE_set_datetime(char * arg);
void list_command(char * arg);
void download_command(char * arg);
void delete_command(char * arg);
void sampling_command(char * arg);
void altitude_command(char * arg);
void set_ntp_server(char * arg);
void set_ntp_timezone_offset(char * arg);
void set_update_server_name(char * arg);
void topic_suffix_config(char * arg);

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
  "get        ",
  "init       ",
  "restore    ",
  "mac        ",
  "method     ",
  "ssid       ",
  "pwd        ",
  "security   ",
  "staticip   ",
  "use        ",
  "mqttsrv    ",
  "mqttport   ",
  "mqttuser   ",
  "mqttpwd    ",  
  "mqttid     ",
  "mqttauth   ",
  "mqttprefix ",
  "mqttsuffix ",
  "updatesrv  ",
  "backup     ",
  "so2_sen    ",
  "so2_slope  ",
  "so2_off    ",
  "o3_sen     ",
  "o3_slope   ",
  "o3_off     ",
  "temp_off   ",
  "hum_off    ",
  "key        ",
  "opmode     ",
  "tempunit   ",
  "updatefile ",
  "force      ",
  "backlight  ",
  "datetime   ",
  "list       ",
  "download   ",
  "delete     ",
  "sampling   ", 
  "altitude   ",
  "ntpsrv     ",
  "tz_off     ",
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
  set_mqtt_topic_prefix,
  topic_suffix_config,
  set_update_server_name,
  backup,
  set_so2_sensitivity,
  set_so2_slope,
  set_so2_offset,
  set_o3_sensitivity,
  set_o3_slope,
  set_o3_offset,
  set_reported_temperature_offset,
  set_reported_humidity_offset,
  set_private_key,
  set_operational_mode,
  set_temperature_units,
  set_update_filename,
  force_command,
  set_backlight_behavior,
  AQE_set_datetime,
  list_command,
  download_command,
  delete_command,
  sampling_command,
  altitude_command,
  set_ntp_server,
  set_ntp_timezone_offset,
  0
};

// tiny watchdog timer intervals
unsigned long previous_tinywdt_millis = 0;
const long tinywdt_interval = 1000;

// sensor sampling timer intervals
unsigned long previous_sensor_sampling_millis = 0;

// touch sampling timer intervals
unsigned long previous_touch_sampling_millis = 0;
const long touch_sampling_interval = 200;

// progress dots timer intervals  
unsigned long previous_progress_dots_millis = 0;
const long progress_dots_interval = 1000;

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

char scratch[1024] = { 0 };  // scratch buffer, for general use
char converted_value_string[64] = {0};
char compensated_value_string[64] = {0};
char raw_value_string[64] = {0};
char MQTT_TOPIC_STRING[128] = {0};
char MQTT_TOPIC_PREFIX[64] = "/orgs/wd/aqe/";
uint8_t mqtt_suffix_enabled = 0;

const char * header_row = "Timestamp,"
               "Temperature[degC],"
               "Humidity[percent],"                                     
               "SO2[ppb],"                    
               "O3[ppb],"      
               "SO2[V]," 
               "O3[V],"  
               "Latitude[deg],"
               "Longitude[deg],"
               "Altitude[m],";         
  
void setup() {
  boolean integrity_check_passed = false;
  boolean mirrored_config_mismatch = false;
  boolean valid_ssid_passed = false; 
  
  // initialize hardware
  initializeHardware(); 
  backlightOff();
      
  //  uint8_t tmp[EEPROM_CONFIG_MEMORY_SIZE] = {0};
  //  get_eeprom_config(tmp);
  //  Serial.println(F("EEPROM Config:"));
  //  dump_config(tmp);
  //  Serial.println();
  //  Serial.println(F("Mirrored Config:"));
  //  get_mirrored_config(tmp);  
  //  dump_config(tmp);
  //  Serial.println();
  
  integrity_check_passed = checkConfigIntegrity();
  // if the integrity check failed, try and undo the damage using the mirror config, if it's valid
  if(!integrity_check_passed){
    Serial.println(F("Info: Startup config integrity check failed, attempting to restore from mirrored configuration."));
    allowed_to_write_config_eeprom = true;
    integrity_check_passed = mirrored_config_restore_and_validate(); 
    allowed_to_write_config_eeprom = false;
  }
  else if(!mirrored_config_matches_eeprom_config()){
    mirrored_config_mismatch = true;
    Serial.println(F("Info: Startup config integrity check passed, but mirrored config differs, attempting to restore from mirrored configuration."));
    allowed_to_write_config_eeprom = true;
    integrity_check_passed = mirrored_config_restore_and_validate();
    allowed_to_write_config_eeprom = false;
  }     
  
  valid_ssid_passed = valid_ssid_config();  
  uint8_t target_mode = eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE);  
  boolean ok_to_exit_config_mode = true;   
  
  // if a software update introduced new settings
  // they should be populated with defaults as necessary
  initializeNewConfigSettings();

  // config mode processing loop
  do{
    // check for initial integrity of configuration in eeprom
    if(mode_requires_wifi(target_mode) && !valid_ssid_passed){
      Serial.println(F("Info: No valid SSID configured, automatically falling back to CONFIG mode."));
      configInject("aqe\r");
      Serial.println();
      setLCD_P(PSTR("PLEASE CONFIGURE"
                    "NETWORK SETTINGS"));
      mode = MODE_CONFIG;
      allowed_to_write_config_eeprom = true;
    }
    else if(!integrity_check_passed && !mirrored_config_mismatch) { 
      // if there was not a mirrored config mismatch and integrity check did not pass
      // that means startup config integrity check failed, and restoring from mirror configuration failed
      // to result in a valid configuration as well
      //
      // if, on the other hand, there was a mirrored config mismatch, the logic above *implies* that the eeprom config 
      // is valid and that the mirrored config is not (yet) valid, so we shouldn't go into this case, and instead 
      // we should drop into the else case (i.e. what normally happens on a startup with a valid configuration)
      Serial.println(F("Info: Config memory integrity check failed, automatically falling back to CONFIG mode."));
      configInject("aqe\r");
      Serial.println();
      setLCD_P(PSTR("CONFIG INTEGRITY"
                    "  CHECK FAILED  "));
      mode = MODE_CONFIG;
      allowed_to_write_config_eeprom = true;   
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
          static uint8_t num_touch_intervals = 0;
          previous_touch_sampling_millis = current_millis;    
          collectTouch();    
          processTouchQuietly();
          
          num_touch_intervals++;
          if(num_touch_intervals == 5){
            petWatchdog(); 
            num_touch_intervals = 0;
          }
          
        }      
      
        if (Serial.available()) {
          if (got_serial_input == false) {
            Serial.println();
          }
          got_serial_input = true;
  
          start = millis(); // reset the timeout
          if (CONFIG_MODE_GOT_INIT == configModeStateMachine(Serial.read(), false)) {
            mode = MODE_CONFIG;
            allowed_to_write_config_eeprom = true;
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
    delayForWatchdog();
    
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
      get_help_indent(); Serial.println(F("...or 'help <cmd>' for help on a specific command"));
  
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
        if(mode != MODE_CONFIG){
          break; // if a command changes mode, we're done with config
        }
        
        if (Serial.available()) {
          idle_time_ms = 0;
          // if you get serial traffic, pass it along to the configModeStateMachine for consumption
          if (CONFIG_MODE_GOT_EXIT == configModeStateMachine(Serial.read(), false)) {
            break;
          }
        }
  
        // pet the watchdog once a second      
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

  allowed_to_write_config_eeprom = false;
  
  Serial.println(F("-~=* In OPERATIONAL Mode *=~-"));
  setLCD_P(PSTR("OPERATIONAL MODE"));
  SUCCESS_MESSAGE_DELAY();
  
  // ... but *which* operational mode are we in?
  mode = target_mode;
  
  // ... and what is the temperature and humdidity offset we should use
  reported_temperature_offset_degC = eeprom_read_float((float *) EEPROM_TEMPERATURE_OFFSET);
  reported_humidity_offset_percent = eeprom_read_float((float *) EEPROM_HUMIDITY_OFFSET);
  
  boolean use_ntp = eeprom_read_byte((uint8_t *) EEPROM_USE_NTP);
  boolean shutdown_wifi = !mode_requires_wifi(mode);
  
  if(mode_requires_wifi(mode) || use_ntp){
    shutdown_wifi = false;
    
    // Scan Networks to show RSSI    
    uint8_t connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);    
    displayRSSI();         
    delayForWatchdog();
    petWatchdog();
    
    // Try and Connect to the Configured Network
    if(!restartWifi()){
      // technically this code should be unreachable
      // because error conditions internal to the restartWifi function
      // should restart the unit at a finer granularity
      // but this additional report should be harmless at any rate
      Serial.println(F("Error: Failed to connect to configured network. Rebooting."));
      Serial.flush();
      watchdogForceReset();
    }
    delayForWatchdog();
    petWatchdog();
  
    // at this point we have connected to the network successfully
    // it's an opportunity to mirror the eeprom configuration
    // if it's different from what's already there
    // importantly this check only happens at startup    
    commitConfigToMirroredConfig();
  
    // Check for Firmware Updates 
    checkForFirmwareUpdates();
    integrity_check_passed = checkConfigIntegrity();
    if(!integrity_check_passed){
      Serial.println(F("Error: Config Integrity Check Failed after checkForFirmwareUpdates"));
      setLCD_P(PSTR("CONFIG INTEGRITY"
                    "  CHECK FAILED  "));
      for(;;){
        // prevent automatic reset
        delay(1000);
        petWatchdog();
      }      
    }
    
    if(use_ntp){
      getNetworkTime();
    }

    if(mode_requires_wifi(mode)){
      // Connect to MQTT server
      if(!mqttReconnect()){
        setLCD_P(PSTR("  MQTT CONNECT  "
                      "     FAILED     "));
        lcdFrownie(15, 1);
        ERROR_MESSAGE_DELAY();      
        Serial.println(F("Error: Unable to connect to MQTT server"));
        Serial.flush();
        watchdogForceReset();    
      }
      delayForWatchdog();
      petWatchdog();
    }
    else{
      shutdown_wifi = true;
    }
  }

  if(shutdown_wifi){
    // it's a mode that doesn't require Wi-Fi
    // save settings as necessary
    commitConfigToMirroredConfig();
    cc3000.stop(); // save power!
  }
  
  // get the temperature units
  temperature_units = eeprom_read_byte((const uint8_t *) EEPROM_TEMPERATURE_UNITS);
  if((temperature_units != 'C') && (temperature_units != 'F')){
    temperature_units = 'C';
  }
  
  // get the sampling, reporting, and averaging parameters
  sampling_interval = eeprom_read_word((uint16_t * ) EEPROM_SAMPLING_INTERVAL) * 1000L;
  reporting_interval = eeprom_read_word((uint16_t * ) EEPROM_REPORTING_INTERVAL) * 1000L;
  sample_buffer_depth = (uint16_t) ((((uint32_t) eeprom_read_word((uint16_t * ) EEPROM_AVERAGING_INTERVAL)) * 1000L) / sampling_interval);
  
  if(mode == SUBMODE_NORMAL){
    setLCD_P(PSTR("TEMP ---  RH ---"
                  "SO2  ---  O3 ---"));           
    SUCCESS_MESSAGE_DELAY();                      
  }
}

void loop() {
  current_millis = millis();

  // whenever you come through loop, process a GPS byte if there is one
  // will need to test if this keeps up, but I think it will
  if(gpsSerial->available()){    
    if(gps.encode(gpsSerial->read())){    
      gps.f_get_position(&gps_latitude, &gps_longitude, &gps_age);
      gps_altitude = gps.f_altitude();
      updateGpsStrings();
    }
  }

  if(current_millis - previous_sensor_sampling_millis >= sampling_interval){
    previous_sensor_sampling_millis = current_millis;    
    //Serial.print(F("Info: Sampling Sensors @ "));
    //Serial.println(millis());
    collectSO2();
    collectO3();
    collectTemperature();
    collectHumidity(); 
    advanceSampleBufferIndex(); 
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
    case SUBMODE_OFFLINE:
      loop_offline_mode();
      break;
    default: // unkown operating mode, nothing to be done 
      break;
  }
  
  // pet the watchdog
  if (current_millis - previous_tinywdt_millis >= tinywdt_interval) {
    previous_tinywdt_millis = current_millis;
    //Serial.println(F("Info: Watchdog Pet."));
    delayForWatchdog();
    petWatchdog();
  }
}

/****** INITIALIZATION SUPPORT FUNCTIONS ******/
void ERROR_MESSAGE_DELAY(void){
  delay(LCD_ERROR_MESSAGE_DELAY);
}

void SUCCESS_MESSAGE_DELAY(void){
  delay(LCD_SUCCESS_MESSAGE_DELAY);
}

void init_firmware_version(void){
  snprintf(firmware_version, 15, "%d.%d.%d", 
    AQEV2FW_MAJOR_VERSION, 
    AQEV2FW_MINOR_VERSION, 
    AQEV2FW_PATCH_VERSION);
}

void initializeHardware(void) {
  wf.begin();
  Serial.begin(115200);

  // gps serial is 9600 baud
  Serial1.begin(9600); // remember must be consistent with global gpsSerial defintion

  init_firmware_version();
  
  // without this line, if the touch hardware is absent
  // serial input processing grinds to a snails pace
  touch.set_CS_Timeout_Millis(100); 

  Serial.println(F(" +------------------------------------+"));
  Serial.println(F(" |   Welcome to Air Quality Egg 2.0   |"));
  Serial.println(F(" |       SO2 / O3 Sensor Suite        |"));  
  Serial.print(F(" |       Firmware Version "));
  Serial.print(firmware_version);
  Serial.println(F("       |"));
  Serial.println(F(" +------------------------------------+"));
  Serial.print(F(" Compiled on: "));
  Serial.println(__DATE__ " " __TIME__);
  Serial.print(F(" Egg Serial Number: "));
  print_eeprom_mqtt_client_id();
  Serial.println();
  
  // Initialize Tiny Watchdog
  Serial.print(F("Info: Tiny Watchdog Initialization..."));
  watchdogInitialize();
  Serial.println(F("OK."));

  pinMode(A6, OUTPUT);
  uint8_t backlight_behavior = eeprom_read_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP);
  if((BACKLIGHT_ON_AT_STARTUP == backlight_behavior) || (backlight_behavior == BACKLIGHT_ALWAYS_ON)){
    backlightOn();
  }
  else{
    backlightOff();
  }

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
  
  byte emptybar[8] = {
          B11111,
          B10001,
          B10001,
          B10001,
          B10001,
          B10001,
          B10001,
          B11111
  };
  
  byte fullbar[8] = {
          B11111,
          B11111,
          B11111,
          B11111,
          B11111,
          B11111,
          B11111,
          B11111
  };     
  
  lcd.begin(16, 2);
  
  lcd.createChar(0, smiley);
  lcd.createChar(1, frownie);  
  lcd.createChar(2, emptybar);
  lcd.createChar(3, fullbar);
  
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
  pinMode(7, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT);
  selectNoSlot();
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
  
  // Initialize SD card
  Serial.print(F("Info: SD Card Initialization..."));        
  if (SD.begin(16)) {
    Serial.println(F("OK."));     
    init_sdcard_ok = true;        
  }
  else{
    Serial.println(F("Fail.")); 
    init_sdcard_ok = false;  
  }

  getCurrentFirmwareSignature();

  // Initialize SHT25
  Serial.print(F("Info: SHT25 Initialization..."));
  if (sht25.begin()) {
    Serial.println(F("OK."));
    init_sht25_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_sht25_ok = false;
  }

  // Initialize SO2 Sensor
  Serial.print(F("Info: SO2 Sensor AFE Initialization..."));
  selectSlot2();
  if (lmp91000.configure(
        LMP91000_TIA_GAIN_120K | LMP91000_RLOAD_10OHM,
        LMP91000_REF_SOURCE_EXT | LMP91000_INT_Z_20PCT
        | LMP91000_BIAS_SIGN_POS | LMP91000_BIAS_8PCT,
        LMP91000_FET_SHORT_DISABLED | LMP91000_OP_MODE_AMPEROMETRIC)) {
    Serial.println(F("OK."));
    init_so2_afe_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_so2_afe_ok = false;
  }

  Serial.print(F("Info: SO2 Sensor ADC Initialization..."));
  if(MCP342x::errorNone == adc.convert(MCP342x::channel1, MCP342x::oneShot, MCP342x::resolution16, MCP342x::gain1)){
    Serial.println(F("OK."));
    init_so2_adc_ok = true;    
  }
  else{
    Serial.println(F("Failed."));
    init_so2_adc_ok = false;    
  }

  Serial.print(F("Info: O3 Sensor AFE Initialization..."));
  selectSlot1();
  if (lmp91000.configure(
        LMP91000_TIA_GAIN_350K | LMP91000_RLOAD_10OHM,
        LMP91000_REF_SOURCE_EXT | LMP91000_INT_Z_67PCT
        | LMP91000_BIAS_SIGN_NEG | LMP91000_BIAS_1PCT,
        LMP91000_FET_SHORT_DISABLED | LMP91000_OP_MODE_AMPEROMETRIC)) {
    Serial.println(F("OK."));
    init_o3_afe_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_o3_afe_ok = false;
  }
  
  Serial.print(F("Info: O3 Sensor ADC Initialization..."));
  if(MCP342x::errorNone == adc.convert(MCP342x::channel1, MCP342x::oneShot, MCP342x::resolution16, MCP342x::gain1)){
    Serial.println(F("OK."));
    init_o3_adc_ok = true;    
  }
  else{
    Serial.println(F("Failed."));
    init_o3_adc_ok = false;    
  }

  // Initialize SD card
  Serial.print(F("Info: RTC Initialization..."));   
  selectSlot3();  
  rtc.begin();
  if (rtc.isrunning()) {
    Serial.println(F("OK."));   
    setSyncProvider(AQE_now);  
    init_rtc_ok = true;       
  }
  else{
    Serial.println(F("Fail.")); 
    init_rtc_ok = false;  
  }

  selectNoSlot();

  uint8_t connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);
  Serial.print(F("Info: CC3000 Initialization..."));  
  SUCCESS_MESSAGE_DELAY(); // don't race past the splash screen, and give watchdog some breathing room
  petWatchdog();
    
  if (cc3000.begin()) {
    Serial.println(F("OK."));
    init_cc3000_ok = true;
  }
  else {
    Serial.println(F("Failed."));
    init_cc3000_ok = false;
  } 

  updateLCD("SO2 / O3", 0);
  updateLCD("MODEL", 1);
  SUCCESS_MESSAGE_DELAY();  
  
}

/****** CONFIGURATION SUPPORT FUNCTIONS ******/
void initializeNewConfigSettings(void){
  static char command_buf[128] = {0};
  boolean in_config_mode = false; 
  allowed_to_write_config_eeprom = true;
  
  // backlight settings
  uint8_t backlight_startup = eeprom_read_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP);
  uint16_t backlight_duration = eeprom_read_word((uint16_t *) EEPROM_BACKLIGHT_DURATION);
  if((backlight_startup == 0xFF) || (backlight_duration == 0xFFFF)){
    configInject("aqe\r");    
    configInject("backlight initon\r");
    configInject("backlight 60\r");
    in_config_mode = true;
  }
  
  // sampling settings
  uint16_t l_sampling_interval = eeprom_read_word((uint16_t * ) EEPROM_SAMPLING_INTERVAL);
  uint16_t l_reporting_interval = eeprom_read_word((uint16_t * ) EEPROM_REPORTING_INTERVAL);
  uint16_t l_averaging_interval = eeprom_read_word((uint16_t * ) EEPROM_AVERAGING_INTERVAL);
  if((l_sampling_interval == 0xFFFF) || (l_reporting_interval == 0xFFFF) || (l_averaging_interval == 0xFFFF)){
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }
    configInject("sampling 5, 160, 5\r");    
  }    

  // the following two blocks of code are a 'hot-fix' to the slope calculation, 
  // only apply it if the slope is not already self consistent with the sensitivity
  float sensitivity = eeprom_read_float((const float *) EEPROM_SO2_SENSITIVITY);  
  float calculated_slope = convert_so2_sensitivity_to_slope(sensitivity);
  float stored_slope = eeprom_read_float((const float *) EEPROM_SO2_CAL_SLOPE);
  if(calculated_slope != stored_slope){ 
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }    
    memset(command_buf, 0, 128);  
    snprintf(command_buf, 127, "so2_sen %8.4f\r", sensitivity);
    configInject(command_buf);
    configInject("backup so2\r");
  }
  
  sensitivity = eeprom_read_float((const float *) EEPROM_O3_SENSITIVITY);  
  calculated_slope = convert_o3_sensitivity_to_slope(sensitivity);
  stored_slope = eeprom_read_float((const float *) EEPROM_O3_CAL_SLOPE);
  if(calculated_slope != stored_slope){ 
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }    
    memset(command_buf, 0, 128);  
    snprintf(command_buf, 127, "o3_sen %8.4f\r", sensitivity);
    configInject(command_buf);  
    configInject("backup o3\r");
  }
  
  // if necessary, initialize the default mqtt prefix
  // if it's never been set, the first byte in memory will be 0xFF
  uint8_t val = eeprom_read_byte((const uint8_t *) EEPROM_MQTT_TOPIC_PREFIX);  
  if(val == 0xFF){
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }    
    memset(command_buf, 0, 128);
    strcat(command_buf, "mqttprefix ");
    strcat(command_buf, MQTT_TOPIC_PREFIX);
    strcat(command_buf, "\r");
    configInject(command_buf);
  }

  // if the mqtt server is set to opensensors.io, change it to mqtt.opensensors.io
  memset(command_buf, 0, 128);
  eeprom_read_block(command_buf, (const void *) EEPROM_MQTT_SERVER_NAME, 31);
  if(strcmp_P(command_buf, PSTR("opensensors.io")) == 0){
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }    
    configInject("mqttsrv mqtt.opensensors.io\r");
  }

  // if the mqtt suffix enable is neither zero nor one, set it to one (enabled)
  val = eeprom_read_byte((const uint8_t *) EEPROM_MQTT_TOPIC_SUFFIX_ENABLED);  
  if(val == 0xFF){
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }    
    memset(command_buf, 0, 128);
    strcat(command_buf, "mqttsuffix enable\r");        
    configInject(command_buf);    
  }
  
  val = eeprom_read_byte((const uint8_t *) EEPROM_2_2_0_SAMPLING_UPD);
  if(val != 1){
    if(!in_config_mode){
      configInject("aqe\r");
      in_config_mode = true;
    }

    configInject("softap enable\r");
    configInject("sampling 5, 600, 60\r");
    eeprom_write_byte((uint8_t *) EEPROM_2_2_0_SAMPLING_UPD, 1);

    // check if ntpsrv is pool.ntp.org, and if so, switch it to 0.airqualityegg.pool.ntp.org
    memset(command_buf, 0, 128);
    eeprom_write_block(command_buf, (void *) EEPROM_NTP_SERVER_NAME, 32);
    if(strcmp(command_buf, "pool.ntp.org") == 0){
      configInject("ntpsrv 0.airqualityegg.pool.ntp.org\r");
    }

    recomputeAndStoreConfigChecksum();
  }

  if(in_config_mode){
    configInject("exit\r");
  }
  
  allowed_to_write_config_eeprom = false;  
}

boolean checkConfigIntegrity(void) {
  uint16_t computed_crc = computeEepromChecksum();
  uint16_t stored_crc = getStoredEepromChecksum();
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
  const uint8_t buf_max_write_idx = 126; // [127] must always have a null-terminator
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

  // if you are at the last write-able location in the buffer
  // the only legal characters to accept are a backspace, a newline, or a carriage return
  // reject anything else implicitly
  if((buf_idx == buf_max_write_idx) && (b != 0x7F) && (b != 0x0D) && (b != 0x0A)){
    Serial.println(F("Warn: Input buffer full and cannot accept new characters. Press enter to clear buffers."));
  }
  // the following logic rejects all non-printable characters besides 0D, 0A, and 7F
  else if (b == 0x7F) { // backspace key is special
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
    
    if (strncmp("aqe", lower_buf, 3) == 0) {
      ret = CONFIG_MODE_GOT_INIT;
    }
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
    boolean got_exit = false;
    got_exit = configModeStateMachine(*str++, reset_buffers);
    if (reset_buffers) {
      reset_buffers = false;
    }
  }
}

void lowercase(char * str) {
  uint16_t len = strlen(str);
  if (len < 0xFFFF) {
    for (uint16_t ii = 0; ii < len; ii++) {
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

void defaults_help_indent(void){
  Serial.print(F("                     "));
}

void get_help_indent(void){
  Serial.print(F("      "));
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
      get_help_indent(); Serial.println(F("exits CONFIG mode and begins OPERATIONAL mode."));
    }
    else if (strncmp("get", arg, 3) == 0) {
      Serial.println(F("get <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("settings - displays all viewable settings"));
      get_help_indent(); Serial.println(F("mac - the MAC address of the cc3000"));
      get_help_indent(); Serial.println(F("method - the Wi-Fi connection method"));
      get_help_indent(); Serial.println(F("ssid - the Wi-Fi SSID to connect to"));
      get_help_indent(); Serial.println(F("pwd - lol, sorry, that's not happening!"));
      get_help_indent(); Serial.println(F("security - the Wi-Fi security mode"));
      get_help_indent(); Serial.println(F("ipmode - the Wi-Fi IP-address mode"));
      get_help_indent(); Serial.println(F("mqttsrv - MQTT server name"));
      get_help_indent(); Serial.println(F("mqttport - MQTT server port"));           
      get_help_indent(); Serial.println(F("mqttuser - MQTT username"));
      get_help_indent(); Serial.println(F("mqttpwd - lol, sorry, that's not happening either!"));      
      get_help_indent(); Serial.println(F("mqttid - MQTT client ID"));      
      get_help_indent(); Serial.println(F("mqttauth - MQTT authentication enabled?"));      
      get_help_indent(); Serial.println(F("updatesrv - Update server name"));      
      get_help_indent(); Serial.println(F("updatefile - Update filename (no extension)"));          
      Serial.println(F("      so2_sen - SO2 sensitivity [nA/ppm]"));
      Serial.println(F("      so2_slope - SO2 sensors slope [ppb/V]"));
      Serial.println(F("      so2_off - SO2 sensors offset [V]"));
      Serial.println(F("      o3_sen - O3 sensitivity [nA/ppm]"));
      Serial.println(F("      o3_slope - O3 sensors slope [ppb/V]"));
      Serial.println(F("      o3_off - O3 sensors offset [V]"));
      get_help_indent(); Serial.println(F("temp_off - Temperature sensor reporting offset [degC] (subtracted)"));
      get_help_indent(); Serial.println(F("hum_off - Humidity sensor reporting offset [%] (subtracted)"));      
      get_help_indent(); Serial.println(F("key - lol, sorry, that's also not happening!"));
      get_help_indent(); Serial.println(F("opmode - the Operational Mode the Egg is configured for"));
      get_help_indent(); Serial.println(F("tempunit - the unit of measure Temperature is reported in (F or C)"));      
      get_help_indent(); Serial.println(F("backlight - the backlight behavior settings (duration, mode)"));
      get_help_indent(); Serial.println(F("timestamp - the current timestamp in the format m/d/y h:m:s"));
      get_help_indent(); Serial.println(F("sampleint - the sensor sampling interval in seconds"));
      get_help_indent(); Serial.println(F("reportint - the reporting sampling interval in seconds"));
      get_help_indent(); Serial.println(F("avgint - the sensor averaging interval in seconds"));
      get_help_indent(); Serial.println(F("altitude - the altitude of the sensor in meters above sea level"));
      get_help_indent(); Serial.println(F("ntpsrv - the NTP server name"));
      get_help_indent(); Serial.println(F("tz_off - the timezone offset for use with NTP in decimal hours"));      
      get_help_indent(); Serial.println(F("result: the current, human-readable, value of <param>"));
      get_help_indent(); Serial.println(F("        is printed to the console."));
    }
    else if (strncmp("init", arg, 4) == 0) {
      Serial.println(F("init <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("mac         - retrieves the mac address from"));
      get_help_indent(); Serial.println(F("                 the CC3000 and stores it in EEPROM"));
    }
    else if (strncmp("restore", arg, 7) == 0) {
      Serial.println(F("restore <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      
      get_help_indent(); Serial.println(F("defaults -   performs:"));
      defaults_help_indent(); Serial.println(F("method direct"));
      defaults_help_indent(); Serial.println(F("security wpa2"));
      defaults_help_indent(); Serial.println(F("use dhcp"));
      defaults_help_indent(); Serial.println(F("opmode normal"));
      defaults_help_indent(); Serial.println(F("tempunit C"));   
      defaults_help_indent(); Serial.println(F("altitude -1")); 
      defaults_help_indent(); Serial.println(F("backlight 60"));
      defaults_help_indent(); Serial.println(F("backlight initon"));      
      defaults_help_indent(); Serial.println(F("mqttsrv mqtt.opensensors.io"));
      defaults_help_indent(); Serial.println(F("mqttport 1883"));           
      defaults_help_indent(); Serial.println(F("mqttauth enable"));        
      defaults_help_indent(); Serial.println(F("mqttuser wickeddevice"));  
      defaults_help_indent(); Serial.println(F("mqttprefix /orgs/wd/aqe/")); 
      defaults_help_indent(); Serial.println(F("mqttsuffix enable")); 
      defaults_help_indent(); Serial.println(F("sampling 5, 160, 5"));
      defaults_help_indent(); Serial.println(F("ntpsrv disable"));
      defaults_help_indent(); Serial.println(F("ntpsrv pool.ntp.org"));
      defaults_help_indent(); Serial.println(F("restore tz_off"));
      defaults_help_indent(); Serial.println(F("restore temp_off"));      
      defaults_help_indent(); Serial.println(F("restore hum_off"));          
      defaults_help_indent(); Serial.println(F("restore mac"));
      defaults_help_indent(); Serial.println(F("restore mqttpwd"));
      defaults_help_indent(); Serial.println(F("restore mqttid"));      
      defaults_help_indent(); Serial.println(F("restore updatesrv"));   
      defaults_help_indent(); Serial.println(F("restore updatefile"));         
      defaults_help_indent(); Serial.println(F("restore key"));
      defaults_help_indent(); Serial.println(F("'restore so2'"));
      defaults_help_indent(); Serial.println(F("'restore o3'"));          
      defaults_help_indent(); Serial.println(F("clears the SSID from memory"));
      defaults_help_indent(); Serial.println(F("clears the Network Password from memory"));
      get_help_indent(); Serial.println(F("mac        - retrieves the mac address from BACKUP"));
      get_help_indent(); Serial.println(F("             and assigns it to the CC3000, via a 'mac' command"));
      get_help_indent(); Serial.println(F("mqttpwd    - restores the MQTT password from BACKUP "));
      get_help_indent(); Serial.println(F("mqttid     - restores the MQTT client ID"));    
      get_help_indent(); Serial.println(F("updatesrv  - restores the Update server name"));          
      get_help_indent(); Serial.println(F("updatefile - restores the Update filename"));           
      get_help_indent(); Serial.println(F("key        - restores the Private Key from BACKUP "));
      get_help_indent(); Serial.println(F("so2        - restores the SO2 calibration parameters from BACKUP "));
      get_help_indent(); Serial.println(F("co         - restores the O3 calibration parameters from BACKUP "));
      get_help_indent(); Serial.println(F("temp_off   - restores the Temperature reporting offset from BACKUP "));
      get_help_indent(); Serial.println(F("hum_off    - restores the Humidity reporting offset from BACKUP "));         
    }
    else if (strncmp("mac", arg, 3) == 0) {
      Serial.println(F("mac <address>"));
      get_help_indent(); Serial.println(F("<address> is a MAC address of the form:"));
      get_help_indent(); Serial.println(F("             08:ab:73:DA:8f:00"));
      get_help_indent(); Serial.println(F("result:  The entered MAC address is assigned to the CC3000"));
      get_help_indent(); Serial.println(F("         and is stored in the EEPROM."));
      warn_could_break_connect();
    }
    else if (strncmp("method", arg, 6) == 0) {
      Serial.println(F("method <type>"));
      get_help_indent(); Serial.println(F("<type> is one of:"));
      get_help_indent(); Serial.println(F("direct - use parameters entered in CONFIG mode"));      
      warn_could_break_connect();      
    }
    else if (strncmp("ssid", arg, 4) == 0) {
      Serial.println(F("ssid <string>"));
      get_help_indent(); Serial.println(F("<string> is the SSID of the network the device should connect to."));
      warn_could_break_connect();      
    }
    else if (strncmp("pwd", arg, 3) == 0) {
      Serial.println(F("pwd <string>"));
      get_help_indent(); Serial.println(F("<string> is the network password for "));
      get_help_indent(); Serial.println(F("the SSID that the device should connect to."));
      warn_could_break_connect();      
    }
    else if (strncmp("security", arg, 8) == 0) {
      Serial.println(F("security <mode>"));
      get_help_indent(); Serial.println(F("<mode> is one of:"));
      get_help_indent(); Serial.println(F("open - the network is unsecured"));
      get_help_indent(); Serial.println(F("wep  - the network WEP security"));
      get_help_indent(); Serial.println(F("wpa  - the network WPA Personal security"));
      get_help_indent(); Serial.println(F("wpa2 - the network WPA2 Personal security"));
      get_help_indent(); Serial.println(F("auto - determine during first network successful scan"));      
      warn_could_break_connect();      
    }
    else if (strncmp("staticip", arg, 8) == 0) {
      Serial.println(F("staticip <config>"));
      Serial.println(F("   <config> is four ip addresses separated by spaces"));
      get_help_indent(); Serial.println(F("<param1> static ip address, e.g. 192.168.1.17"));
      get_help_indent(); Serial.println(F("<param2> netmask, e.g. 255.255.255.0"));      
      get_help_indent(); Serial.println(F("<param3> default gateway ip address, e.g. 192.168.1.1"));      
      get_help_indent(); Serial.println(F("<param4> dns server ip address, e.g. 8.8.8.8"));      
      get_help_indent(); Serial.println(F("result: The entered static network parameters will be used by the CC3000"));
      get_help_indent(); Serial.println(F("note:   To configure DHCP use command 'use dhcp'"));
      warn_could_break_connect();      
    }
    else if (strncmp("use", arg, 3) == 0) {
      Serial.println(F("use <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("dhcp - wipes the Static IP address from the EEPROM"));
      get_help_indent(); Serial.println(F("ntp - enables NTP capabilities"));
      warn_could_break_connect();      
    }
    else if (strncmp("list", arg, 4) == 0) {
      Serial.println(F("list <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("files - lists all files on the sd card (if inserted)"));   
    }
    else if (strncmp("download", arg, 8) == 0) {
      Serial.println(F("download <filename>"));
      get_help_indent(); Serial.println(F("prints the contents of the named file to the console."));
      Serial.println(F("download <YYMMDDHH> <YYMMDDHH>"));      
      get_help_indent(); Serial.println(F("prints the contents of files from start to end dates inclusive."));
    }   
    else if (strncmp("delete", arg, 6) == 0) {
      Serial.println(F("delete <filename>"));
      get_help_indent(); Serial.println(F("deletes the named file from the SD card."));
      Serial.println(F("delete <YYMMDDHH> <YYMMDDHH>"));      
      get_help_indent(); Serial.println(F("deletes the files from start to end dates inclusive."));
    }       
    else if (strncmp("force", arg, 5) == 0) {
      Serial.println(F("force <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("update - invalidates the firmware signature, "));
      get_help_indent(); Serial.println(F("         configures for normal mode, and exits config mode, "));
      get_help_indent(); Serial.println(F("         and should initiate firmware update after connecting to wi-fi."));
    }    
    else if (strncmp("sampling", arg, 8) == 0) {
      Serial.println(F("sampling <sampling_interval>, <averaging_interval>, <reporting_interval>"));
      get_help_indent(); Serial.println(F("<sampling_interval> is the duration between sensor samples, in integer seconds (at least 3)"));
      get_help_indent(); Serial.println(F("<averaging_interval> is the duration that is averaged in reporting, in seconds (multiple of sample interval)"));
      get_help_indent(); Serial.println(F("<reporting_interval> is the duration between sensor reports (wifi/console/sd/etc.), in seconds (multiple of sample interval)"));
    }
    else if (strncmp("mqttpwd", arg, 7) == 0) {
      Serial.println(F("mqttpwd <string>"));
      get_help_indent(); Serial.println(F("<string> is the password the device will use to connect "));
      get_help_indent(); Serial.println(F("to the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();      
    }
    else if (strncmp("mqttsrv", arg, 7) == 0) {
      Serial.println(F("mqttsrv <string>"));
      get_help_indent(); Serial.println(F("<string> is the DNS name of the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }
    else if (strncmp("mqttuser", arg, 8) == 0) {
      Serial.println(F("mqttuser <string>"));
      get_help_indent(); Serial.println(F("<string> is the username used to connect to the MQTT server."));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }
    else if (strncmp("mqttid", arg, 6) == 0) {
      Serial.println(F("mqttid <string>"));
      get_help_indent(); Serial.println(F("<string> is the Client ID used to connect to the MQTT server."));
      get_help_indent(); Serial.println(F("Must be between 1 and 23 characters long per MQTT v3.1 spec."));    
      // Ref: http://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html#connect  
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }    
    else if (strncmp("mqttauth", arg, 8) == 0) {
      Serial.println(F("mqttauth <string>"));
      get_help_indent(); Serial.println(F("<string> is the one of 'enable' or 'disable'"));
      note_know_what_youre_doing();
      warn_could_break_upload();   
    }        
    else if (strncmp("mqttprefix", arg, 10) == 0) {
      Serial.println(F("mqttprefix <string>"));
      get_help_indent(); Serial.println(F("<string> is pre-pended to the logical topic for each sensor"));
      note_know_what_youre_doing();
      warn_could_break_upload();   
    }        
    else if (strncmp("mqttsuffix", arg, 10) == 0) {
      Serial.println(F("mqttsuffix <string>"));
      get_help_indent(); Serial.println(F("<string> is 'enable' or 'disable'"));
      get_help_indent(); Serial.println(F("if enabled, appends the device ID to each topic"));
      note_know_what_youre_doing();
      warn_could_break_upload();   
    }    
    else if (strncmp("mqttport", arg, 8) == 0) {
      Serial.println(F("mqttport <number>"));
      get_help_indent(); Serial.println(F("<number> is the a number between 1 and 65535 inclusive"));
      note_know_what_youre_doing();
      warn_could_break_upload();  
    }            
    else if (strncmp("updatesrv", arg, 9) == 0) {
      Serial.println(F("updatesrv <string>"));
      get_help_indent(); Serial.println(F("<string> is the DNS name of the Update server."));
      get_help_indent(); Serial.println(F("note: to disable internet firmware updates type 'updatesrv disable'"));
      get_help_indent(); Serial.println(F("      to re-enable internet firmware updates type 'restore updatesrv'"));
      get_help_indent(); Serial.println(F("warning: Using this command incorrectly can prevent your device"));
      get_help_indent(); Serial.println(F("         from getting firmware updates over the internet."));
    }   
    else if (strncmp("ntpsrv", arg, 6) == 0) {
      Serial.println(F("ntpsrv <string>"));
      get_help_indent(); Serial.println(F("<string> is the DNS name of the Update server."));
      get_help_indent(); Serial.println(F("note: to disable NTP type 'ntpsrv disable'"));      
    }   
    else if (strncmp("updatefile", arg, 10) == 0) {
      Serial.println(F("updatefile <string>"));
      get_help_indent(); Serial.println(F("<string> is the filename to load from the Update server, excluding the extension."));
      get_help_indent(); Serial.println(F("note:    Unless you *really* know what you're doing, you should"));
      get_help_indent(); Serial.println(F("         probably not be using this command."));
      get_help_indent(); Serial.println(F("warning: Using this command incorrectly can prevent your device"));
      get_help_indent(); Serial.println(F("         from getting firmware updates over the internet."));
    }    
    else if (strncmp("backup", arg, 6) == 0) {
      Serial.println(F("backup <param>"));
      get_help_indent(); Serial.println(F("<param> is one of:"));
      get_help_indent(); Serial.println(F("mqttpwd  - backs up the MQTT password"));
      get_help_indent(); Serial.println(F("mac      - backs up the CC3000 MAC address"));
      get_help_indent(); Serial.println(F("key      - backs up the 256-bit private key"));
      get_help_indent(); Serial.println(F("no2      - backs up the NO2 calibration parameters"));
      get_help_indent(); Serial.println(F("co       - backs up the CO calibration parameters"));
      get_help_indent(); Serial.println(F("temp     - backs up the Temperature calibration parameters"));
      get_help_indent(); Serial.println(F("hum      - backs up the Humidity calibration parameters"));  
      get_help_indent(); Serial.println(F("tz       - backs up the Timezone offset for NTP"));    
      get_help_indent(); Serial.println(F("all      - does all of the above"));
    }
    else if (strncmp("so2_sen", arg, 7) == 0) {
      Serial.println(F("so2_sen <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of SO2 sensitivity [nA/ppm]"));
      get_help_indent(); Serial.println(F("note: also sets the SO2 slope based on the sensitivity"));
    }
    else if (strncmp("so2_slope", arg, 9) == 0) {
      Serial.println(F("so2_slope <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of SO2 sensor slope [ppb/V]"));
    }
    else if (strncmp("so2_off", arg, 7) == 0) {
      Serial.println(F("so2_off <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of SO2 sensor offset [V]"));
    }
    else if (strncmp("o3_sen", arg, 6) == 0) {
      Serial.println(F("o3_sen <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of O3 sensitivity [nA/ppm]"));
      get_help_indent(); Serial.println(F("note: also sets the O3 slope based on the sensitivity"));      
    }
    else if (strncmp("o3_slope", arg, 8) == 0) {
      Serial.println(F("o3_slope <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of O3 sensor slope [ppb/V]"));
    }
    else if (strncmp("o3_off", arg, 6) == 0) {
      Serial.println(F("o3_off <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of O3 sensor offset [V]"));
    }
    else if (strncmp("temp_off", arg, 8) == 0) {
      Serial.println(F("temp_off <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of Temperature sensor reporting offset [degC] (subtracted)"));
    }
    else if (strncmp("hum_off", arg, 7) == 0) {
      Serial.println(F("hum_off <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of Humidity sensor reporting offset [%] (subtracted)"));
    }    
    else if (strncmp("key", arg, 3) == 0) {
      Serial.println(F("key <string>"));
      get_help_indent(); Serial.println(F("<string> is a 64-character string representing "));
      get_help_indent(); Serial.println(F("a 32-byte (256-bit) hexadecimal value of the private key"));
    }
    else if (strncmp("opmode", arg, 6) == 0) {
      Serial.println(F("opmode <mode>"));
      Serial.println(F("   <mode> is one of:"));
      Serial.println(F("      normal - publish data to MQTT server over Wi-Fi"));
      Serial.println(F("      offline - this mode writes data to an installed microSD card, creating one file per hour, "));
      Serial.println(F("                named by convention YYMMDDHH.csv, intended to be used in conjunction with RTC module"));
    }    
    else if (strncmp("tempunit", arg, 8) == 0) {
      Serial.println(F("tempunit <unit>"));
      get_help_indent(); Serial.println(F("<unit> is one of:"));      
      get_help_indent(); Serial.println(F("C - report temperature in Celsius"));
      get_help_indent(); Serial.println(F("F - report temperature in Fahrenheit"));
    }
    else if (strncmp("altitude", arg, 8) == 0) {
      Serial.println(F("altitude <number>"));
      get_help_indent(); Serial.println(F("<number> is the height above sea level where the Egg is [meters]"));   
      get_help_indent(); Serial.println(F("Note: -1 is a special value meaning do not apply pressure compensation"));
    }    
    else if (strncmp("datetime", arg, 8) == 0) {
      Serial.println(F("datetime <csv-date-time>"));
      get_help_indent(); Serial.println(F("<csv-date-time> is a comma separated date in the order year, month, day, hours, minutes, seconds"));    
    }
    else if(strncmp("tz_off", arg, 6) == 0){
      Serial.println(F("tz_off <number>"));
      get_help_indent(); Serial.println(F("<number> is the decimal value of the timezone offset in hours from GMT"));
    }
    else if (strncmp("backlight", arg, 9) == 0){
      Serial.println(F("backlight <config>"));
      get_help_indent(); Serial.println(F("<config> is one of:"));      
      get_help_indent(); Serial.println(F("<seconds> - the whole number interval, in seconds, to keep the backlight on for when engaged"));
      get_help_indent(); Serial.println(F("initoff - makes the backlight off at startup"));      
      get_help_indent(); Serial.println(F("initon - makes the backlight on at startup"));            
      get_help_indent(); Serial.println(F("alwayson - makes the backlight always on"));            
      get_help_indent(); Serial.println(F("alwaysoff - makes the backlight always off"));                  
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

  uint8_t connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);    
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
    case WLAN_SEC_AUTO:
      Serial.println(F("Automatic - Not Yet Determined"));
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

void print_eeprom_string(const char * address, const char * unless_it_matches_this, const char * in_which_case_print_this_instead){
  char tmp[32] = {0};
  eeprom_read_block(tmp, (const void *) address, 31);

  if(strcmp(tmp, unless_it_matches_this) == 0){
    Serial.println(in_which_case_print_this_instead);
  }
  else{
    Serial.println(tmp);
  }
}

void print_eeprom_update_server(){
  print_eeprom_string((const char *) EEPROM_UPDATE_SERVER_NAME, "", "Disabled");
}

void print_eeprom_ntp_server(){
  print_eeprom_string((const char *) EEPROM_NTP_SERVER_NAME, "", "Disabled");
}

void print_eeprom_update_filename(){
  print_eeprom_string((const char *) EEPROM_UPDATE_FILENAME);
}  

void print_eeprom_mqtt_server(){
  print_eeprom_string((const char *) EEPROM_MQTT_SERVER_NAME);
}


void print_eeprom_mqtt_client_id(){
  print_eeprom_string((const char *) EEPROM_MQTT_CLIENT_ID);
}

void print_eeprom_mqtt_topic_prefix(){
  print_eeprom_string((const char *) EEPROM_MQTT_TOPIC_PREFIX);
}

void print_eeprom_mqtt_topic_suffix(){
  uint8_t val = eeprom_read_byte((uint8_t * ) EEPROM_MQTT_TOPIC_SUFFIX_ENABLED);
  if(val == 1){
    Serial.print("Enabled");
  }
  else if(val == 0){
    Serial.print("Disabled");
  }
  else{
    Serial.print("Uninitialized");
  }
  Serial.println();
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
    case SUBMODE_OFFLINE:
      Serial.println(F("Offline"));
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

void print_eeprom_temperature_units(){   
  uint8_t tempunit = eeprom_read_byte((uint8_t *) EEPROM_TEMPERATURE_UNITS);
  switch (tempunit) {
    case 'C':
      Serial.println(F("Celsius"));
      break;
    case 'F':
      Serial.println(F("Fahrenheit"));
      break;
    default:
      Serial.print(F("Error: Unknown temperature units [0x"));
      if (tempunit < 0x10) {
        Serial.print(F("0"));
      }
      Serial.print(tempunit, HEX);
      Serial.println(F("]"));
      break;
  }  
}

void print_altitude_settings(void){
  int16_t l_altitude = (int16_t) eeprom_read_word((uint16_t *) EEPROM_ALTITUDE_METERS);
  if(l_altitude != -1){
    Serial.print(l_altitude);
    Serial.println(F(" meters"));  
  }
  else{
    Serial.println("Not set");
  }
}

void print_eeprom_backlight(){   
  uint16_t backlight_duration = eeprom_read_word((uint16_t *) EEPROM_BACKLIGHT_DURATION);
  uint8_t backlight_startup = eeprom_read_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP);
  Serial.print(backlight_duration);
  Serial.print(F(" seconds, "));
  switch(backlight_startup){
    case BACKLIGHT_ON_AT_STARTUP:
      Serial.println(F("ON at startup"));
      break;
    case BACKLIGHT_OFF_AT_STARTUP:
      Serial.println(F("OFF at startup"));    
      break;
    case BACKLIGHT_ALWAYS_ON:
      Serial.println(F("always ON"));    
      break;
    case BACKLIGHT_ALWAYS_OFF:
      Serial.println(F("always OFF"));        
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
  else if (strncmp(arg, "so2_sen", 7) == 0) {
    print_eeprom_float((const float *) EEPROM_SO2_SENSITIVITY);
  }
  else if (strncmp(arg, "so2_slope", 9) == 0) {
    print_eeprom_float((const float *) EEPROM_SO2_CAL_SLOPE);
  }
  else if (strncmp(arg, "so2_off", 7) == 0) {
    print_eeprom_float((const float *) EEPROM_SO2_CAL_OFFSET);
  }
  else if (strncmp(arg, "o3_sen", 6) == 0) {
    print_eeprom_float((const float *) EEPROM_O3_SENSITIVITY);
  }
  else if (strncmp(arg, "o3_slope", 8) == 0) {
    print_eeprom_float((const float *) EEPROM_O3_CAL_SLOPE);
  }
  else if (strncmp(arg, "o3_off", 6) == 0) {
    print_eeprom_float((const float *) EEPROM_O3_CAL_OFFSET);
  }
  else if (strncmp(arg, "temp_off", 8) == 0) {
    print_eeprom_float((const float *) EEPROM_TEMPERATURE_OFFSET);
  }
  else if (strncmp(arg, "hum_off", 7) == 0) {
    print_eeprom_float((const float *) EEPROM_HUMIDITY_OFFSET);
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
  else if(strncmp(arg, "tempunit", 8) == 0) {
     print_eeprom_temperature_units();
  }
  else if(strncmp(arg, "backlight", 9) == 0) {
    print_eeprom_backlight();
  }
  else if(strncmp(arg, "timestamp", 9) == 0) {
    printCurrentTimestamp(NULL, NULL);
    Serial.println();
  } 
  else if(strncmp(arg, "updatesrv", 9) == 0) {
    print_eeprom_update_server();    
  }  
  else if(strncmp(arg, "ntpsrv", 6) == 0) {
    print_eeprom_ntp_server();    
  }  
  else if(strncmp(arg, "updatefile", 10) == 0) {
    print_eeprom_string((const char *) EEPROM_UPDATE_FILENAME);    
  }  
  else if(strncmp(arg, "sampleint", 9) == 0) {
    Serial.println(eeprom_read_word((uint16_t *) EEPROM_SAMPLING_INTERVAL));    
  }   
  else if(strncmp(arg, "reportint", 9) == 0) {
    Serial.println(eeprom_read_word((uint16_t *) EEPROM_REPORTING_INTERVAL));    
  }     
  else if(strncmp(arg, "avgint", 6) == 0) {
    Serial.println(eeprom_read_word((uint16_t *) EEPROM_AVERAGING_INTERVAL));    
  } 
  else if(strncmp(arg, "altitude", 8) == 0) {
    Serial.println((int16_t) eeprom_read_word((uint16_t *) EEPROM_ALTITUDE_METERS));    
  }         
  else if(strncmp(arg, "settings", 8) == 0) {
    static char allff[64] = {0};
    memset(allff, 0xff, 64);

    // print all the settings to the screen in an orderly fashion
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Preferences/Options:                                        |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.print(F("    Operational Mode: "));
    print_eeprom_operational_mode(eeprom_read_byte((const uint8_t *) EEPROM_OPERATIONAL_MODE));
    Serial.print(F("    Temperature Units: "));
    print_eeprom_temperature_units();
    Serial.print(F("    Altitude: "));
    print_altitude_settings();
    Serial.print(F("    Backlight Settings: "));
    print_eeprom_backlight();
    Serial.print(F("    Sensor Sampling Interval: "));
    Serial.print(eeprom_read_word((uint16_t *) EEPROM_SAMPLING_INTERVAL));  
    Serial.println(F(" seconds"));
    Serial.print(F("    Sensor Averaging Interval: "));
    Serial.print(eeprom_read_word((uint16_t *) EEPROM_AVERAGING_INTERVAL));  
    Serial.println(F(" seconds"));
    Serial.print(F("    Sensor Reporting Interval: "));
    Serial.print(eeprom_read_word((uint16_t *) EEPROM_REPORTING_INTERVAL));  
    Serial.println(F(" seconds"));
    
    Serial.println(F(" +-------------------------------------------------------------+"));
    Serial.println(F(" | Network Settings:                                           |"));
    Serial.println(F(" +-------------------------------------------------------------+"));
    print_label_with_star_if_not_backed_up("MAC Address: ", BACKUP_STATUS_MAC_ADDRESS_BIT);
    print_eeprom_mac();
    uint8_t connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);
    Serial.print(F("    Method: "));    
    print_eeprom_connect_method();
    Serial.print(F("    SSID: "));
    print_eeprom_ssid();
    Serial.print(F("    Security Mode: "));
    print_eeprom_security_type();
    Serial.print(F("    IP Mode: "));
    print_eeprom_ipmode();
    Serial.print(F("    Update Server: "));
    print_eeprom_update_server();    
    Serial.print(F("    Update Filename: "));
    print_eeprom_update_filename();        
    Serial.print(F("    NTP Server: "));
    if(eeprom_read_byte((uint8_t *) EEPROM_USE_NTP) == 1){
      print_eeprom_ntp_server();
    }
    else{
      Serial.println(F("Disabled"));
    }
    print_label_with_star_if_not_backed_up("NTP TZ Offset: ", BACKUP_STATUS_TIMEZONE_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_NTP_TZ_OFFSET_HRS);
    
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
    Serial.print(F("    MQTT Topic Prefix: "));
    print_eeprom_mqtt_topic_prefix();
    Serial.print(F("    MQTT Topic Suffix: "));
    print_eeprom_mqtt_topic_suffix();      
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

    print_label_with_star_if_not_backed_up("SO2 Sensitivity [nA/ppm]: ", BACKUP_STATUS_SO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_SO2_SENSITIVITY);
    print_label_with_star_if_not_backed_up("SO2 Slope [ppb/V]: ", BACKUP_STATUS_SO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_SO2_CAL_SLOPE);
    print_label_with_star_if_not_backed_up("SO2 Offset [V]: ", BACKUP_STATUS_SO2_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_SO2_CAL_OFFSET);

    print_label_with_star_if_not_backed_up("O3 Sensitivity [nA/ppm]: ", BACKUP_STATUS_O3_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_O3_SENSITIVITY);
    print_label_with_star_if_not_backed_up("O3 Slope [ppb/V]: ", BACKUP_STATUS_O3_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_O3_CAL_SLOPE);
    print_label_with_star_if_not_backed_up("O3 Offset [V]: ", BACKUP_STATUS_O3_CALIBRATION_BIT);
    print_eeprom_float((const float *) EEPROM_O3_CAL_OFFSET);
    
    char temp_reporting_offset_label[64] = {0};
    char temperature_units = (char) eeprom_read_byte((uint8_t *) EEPROM_TEMPERATURE_UNITS);
    snprintf(temp_reporting_offset_label, 63, "Temperature Reporting Offset [deg%c]: ", temperature_units); 
    float temp_reporting_offset_degc = eeprom_read_float((float *) EEPROM_TEMPERATURE_OFFSET);
    float temperature_offset_display = temp_reporting_offset_degc;
    if(temperature_units == 'F'){
      temperature_offset_display = toFahrenheit(temp_reporting_offset_degc) - 32.0f;
    }
    print_label_with_star_if_not_backed_up((char * )temp_reporting_offset_label, BACKUP_STATUS_TEMPERATURE_CALIBRATION_BIT);
    Serial.println(temperature_offset_display, 2);
    
    print_label_with_star_if_not_backed_up("Humidity Reporting Offset [%]: ", BACKUP_STATUS_HUMIDITY_CALIBRATION_BIT);
    Serial.println(eeprom_read_float((float *) EEPROM_HUMIDITY_OFFSET), 2);  
    
    
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }

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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  char blank[32] = {0};
  uint8_t tmp[32] = {0};
  boolean valid = true;

  // things that must have been backed up before restoring.
  // 1. MAC address              0x80
  // 2. MQTT Password            0x40
  // 3. Private Key              0x20
  // 4. SO2 Calibration Values   0x10
  // 5. O3 Calibratino Values    0x80

  uint16_t backup_check = eeprom_read_word((const uint16_t *) EEPROM_BACKUP_CHECK);

  if (strncmp(arg, "defaults", 8) == 0) {
    prompt();
    configInject("method direct\r");
    configInject("security auto\r");
    configInject("use dhcp\r");
    configInject("opmode normal\r");
    configInject("tempunit C\r");    
    configInject("altitude -1\r");    
    configInject("backlight 60\r");
    configInject("backlight initon\r");
    configInject("mqttsrv mqtt.opensensors.io\r");
    configInject("mqttport 1883\r");        
    configInject("mqttauth enable\r");    
    configInject("mqttuser wickeddevice\r");
    configInject("mqttprefix /orgs/wd/aqe/\r");
    configInject("mqttsuffix enable\r");
    configInject("sampling 5, 160, 5\r");   
    configInject("ntpsrv disable\r");
    configInject("ntpsrv pool.ntp.org\r");
    configInject("restore tz_off\r");
    configInject("restore temp_off\r");
    configInject("restore hum_off\r");       
    configInject("restore mqttpwd\r");
    configInject("restore mqttid\r");
    configInject("restore updatesrv\r");
    configInject("restore updatefile\r");    
    configInject("restore key\r");
    configInject("restore so2\r");
    configInject("restore o3\r");
    configInject("restore mac\r");

    eeprom_write_block(blank, (void *) EEPROM_SSID, 32); // clear the SSID
    eeprom_write_block(blank, (void *) EEPROM_NETWORK_PWD, 32); // clear the Network Password
    mirrored_config_erase(); // erase the mirrored configuration, which will be restored next successful network connect                            
    
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

    lowercase((char *) tmp); // for consistency with aqe v1 and airqualityegg.com assumptions
    
    eeprom_write_block(tmp, (void *) EEPROM_MQTT_CLIENT_ID, 32);
  }  
  else if (strncmp("updatesrv", arg, 9) == 0) {
    eeprom_write_block("update.wickeddevice.com", (void *) EEPROM_UPDATE_SERVER_NAME, 32);
  }  
  else if (strncmp("updatefile", arg, 10) == 0) {
    eeprom_write_block("aqev2_so2_o3", (void *) EEPROM_UPDATE_FILENAME, 32);
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
  else if (strncmp("so2", arg, 3) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_SO2_CALIBRATION_BIT)) {
      Serial.println(F("Error: SO2 calibration must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_SO2_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_SO2_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_SO2_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_SO2_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_SO2_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_SO2_CAL_OFFSET, 4);
  }
  else if (strncmp("o3", arg, 2) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_O3_CALIBRATION_BIT)) {
      Serial.println(F("Error: O3 calibration must be backed up  "));
      Serial.println(F("       prior to executing a 'restore'."));
      return;
    }

    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_O3_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_O3_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_O3_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_O3_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_O3_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_O3_CAL_OFFSET, 4);
  }
  else if (strncmp("temp_off", arg, 8) == 0) {
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_TEMPERATURE_CALIBRATION_BIT)) {
      Serial.println(F("Error: Temperature reporting offset should be backed up  "));
      Serial.println(F("       prior to executing a 'restore'. Setting to 0.0"));
      eeprom_write_float((float *) EEPROM_TEMPERATURE_OFFSET, 0.0f);  
    }
    else{
      eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_TEMPERATURE_OFFSET, 4);
      eeprom_write_block(tmp, (void *) EEPROM_TEMPERATURE_OFFSET, 4);
    }
  }
  else if (strncmp("hum_off", arg, 7) == 0) {   
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_HUMIDITY_CALIBRATION_BIT)) {
      Serial.println(F("Warning: Humidity reporting offset should be backed up  "));
      Serial.println(F("         prior to executing a 'restore'. Setting to 0.0."));   
      eeprom_write_float((float *) EEPROM_HUMIDITY_OFFSET, 0.0f);
    }
    else{
      eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_HUMIDITY_OFFSET, 4);
      eeprom_write_block(tmp, (void *) EEPROM_HUMIDITY_OFFSET, 4);
    }
  }
  else if(strncmp("tz_off", arg, 6) == 0) {   
    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_TIMEZONE_CALIBRATION_BIT)) {
      Serial.println(F("Warning: Timezone offset should be backed up  "));
      Serial.println(F("         prior to executing a 'restore'. Setting to 0.0."));   
      eeprom_write_float((float *) EEPROM_NTP_TZ_OFFSET_HRS, 0.0f);
    }
    else{
      eeprom_read_block(tmp, (const void *) EEPROM_BACKUP_NTP_TZ_OFFSET_HRS, 4);
      eeprom_write_block(tmp, (void *) EEPROM_NTP_TZ_OFFSET_HRS, 4);
    }
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

void set_backlight_behavior(char * arg){
  boolean valid = true;  
  
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  lowercase(arg);
  
  if(strncmp(arg, "initon", 6) == 0){
    eeprom_write_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP, BACKLIGHT_ON_AT_STARTUP);
  }
  else if(strncmp(arg, "initoff", 7) == 0){
    eeprom_write_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP, BACKLIGHT_OFF_AT_STARTUP);
  }
  else if(strncmp(arg, "alwayson", 8) == 0){
    eeprom_write_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP, BACKLIGHT_ALWAYS_ON);
  }
  else if(strncmp(arg, "alwaysoff", 9) == 0){
    eeprom_write_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP, BACKLIGHT_ALWAYS_OFF);
  }  
  else{ 
    boolean arg_contains_only_digits = true;
    char * ptr = arg;
    uint16_t arglen = strlen(arg);    
    for(uint16_t ii = 0; ii < arglen; ii++){
      if(!isdigit(arg[ii])){
        arg_contains_only_digits = true;
        break; 
      }
    }
    
    if(arg_contains_only_digits){
      uint32_t duration = (uint32_t) strtoul(arg, NULL, 10);
      if(duration < 0xFFFF){
        eeprom_write_word((uint16_t *) EEPROM_BACKLIGHT_DURATION, (uint16_t) duration);
      }
    }
    else{
      valid = false;
      Serial.print(F("Error: Unexpected paramater name \""));
      Serial.print(arg);
      Serial.println(F("\""));      
    }
  }
  
  if (valid) {
    recomputeAndStoreConfigChecksum();
  }  
}

void altitude_command(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }

  char * endptr = NULL;
  trim_string(arg);
  int16_t l_altitude = (int16_t) strtol(arg, &endptr, 10);
  if(*endptr == NULL){
    eeprom_write_word((uint16_t *) EEPROM_ALTITUDE_METERS, l_altitude);
    recomputeAndStoreConfigChecksum();
  }
  else{
    Serial.println(F("Error: altitude must be a numeric"));
  }
}

void sampling_command(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  } 

  uint16_t len = strlen(arg);
  uint8_t num_commas = 0;  
  
  for(uint16_t ii = 0; ii < len; ii++){
    // if any character is not a space, comma, or digit it's unparseable
    if((!isdigit(arg[ii])) && (arg[ii] != ' ') && (arg[ii] != ',')){
      Serial.print(F("Error: Found invalid character '"));
      Serial.print((char) arg[ii]);
      Serial.print(F("'"));
      Serial.println();
      return;
    }

    if(arg[ii] == ','){
      num_commas++;
    }
  }    
    
  if(num_commas != 2){
    Serial.print(F("Error: sampling expects exactly 3 values separated by commas, but received "));
    Serial.print(num_commas - 1);
    Serial.println();
    return; 
  }  
  
  // ok we have 3 numeric arguments separated by commas, parse them
  // ok we have six numeric arguments separated by commas, parse them
  char tmp[32] = {0};  
  strncpy(tmp, arg, 31); // copy the string so you don't mutilate the argument
  char * token = strtok(tmp, ",");
  uint8_t token_number = 0;  
  uint16_t l_sample_interval = 0;
  uint16_t l_averaging_interval = 0;
  uint16_t l_reporting_interval = 0;
  
  while (token != NULL) {
    switch(token_number++){
      case 0:
        l_sample_interval = (uint16_t) strtoul(token, NULL, 10);
        if(l_sample_interval < 3){
          Serial.print(F("Error: Sampling interval must be greater than 2 [was ")); 
          Serial.print(l_sample_interval);
          Serial.print(F("]"));
          Serial.println();
          return;
        }
        break;      
      case 1:
        l_averaging_interval = (uint16_t) strtoul(token, NULL, 10);
        if(l_averaging_interval < 1){
          Serial.print(F("Error: Averaging interval must be greater than 0 [was ")); 
          Serial.print(l_averaging_interval);
          Serial.print(F("]"));
          Serial.println();
          return;
        }
        else if((l_averaging_interval % l_sample_interval) != 0){
          Serial.print(F("Error: Averaging interval must be an integer multiple of the Sampling interval"));
          Serial.println();
          return;          
        }
        else if((l_averaging_interval / l_sample_interval) > MAX_SAMPLE_BUFFER_DEPTH){
          Serial.print(F("Error: Insufficient memory available for averaging interval @ sampling interval."));
          Serial.print(F("       Must require no more than "));          
          Serial.print(MAX_SAMPLE_BUFFER_DEPTH);
          Serial.print(F(" samples, but requires"));
          Serial.print(l_averaging_interval / l_sample_interval);
          Serial.println(F(" samples"));
          Serial.println();
          return;                    
        }
        break;
      case 2:
        l_reporting_interval = (uint16_t) strtoul(token, NULL, 10);
        if(l_reporting_interval < 1){
          Serial.print(F("Error: Reporting interval must be greater than 0 [was ")); 
          Serial.print(l_reporting_interval);
          Serial.print(F("]"));
          Serial.println();
          return;
        }
        else if((l_reporting_interval % l_sample_interval) != 0){
          Serial.print(F("Error: Reporting interval must be an integer multiple of the Sampling interval"));
          Serial.println();
          return;          
        }        
        break;
    }    
    
    token = strtok(NULL, ",");
  } 
  
  // we got through all the checks! save these parameters to config memory
  eeprom_write_word((uint16_t *) EEPROM_SAMPLING_INTERVAL, l_sample_interval);
  eeprom_write_word((uint16_t *) EEPROM_REPORTING_INTERVAL, l_reporting_interval);
  eeprom_write_word((uint16_t *) EEPROM_AVERAGING_INTERVAL, l_averaging_interval);
  recomputeAndStoreConfigChecksum();  
}

void AQE_set_datetime(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  } 
  
  uint16_t len = strlen(arg);
  uint8_t num_commas = 0;  
  
  for(uint16_t ii = 0; ii < len; ii++){
    // if any character is not a space, comma, or digit it's unparseable
    if((!isdigit(arg[ii])) && (arg[ii] != ' ') && (arg[ii] != ',')){
      Serial.print(F("Error: Found invalid character '"));
      Serial.print((char) arg[ii]);
      Serial.print(F("'"));
      Serial.println();
      return;
    }
    
    if(arg[ii] == ','){
      num_commas++;
    }
  }
 
  if(num_commas != 5){
    Serial.print(F("Error: datetime expects exactly 6 values separated by commas, but received "));
    Serial.print(num_commas - 1);
    Serial.println();
    return; 
  }
  
  // ok we have six numeric arguments separated by commas, parse them
  char tmp[32] = {0};  
  strncpy(tmp, arg, 31); // copy the string so you don't mutilate the argument
  char * token = strtok(tmp, ",");
  uint8_t token_number = 0;  
  uint8_t mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
  uint16_t yr = 0;
  
  while (token != NULL) {
    switch(token_number++){
      case 0:
        yr = (uint16_t) strtoul(token, NULL, 10);
        if(yr < 2015){
          Serial.print(F("Error: Year must be no earlier than 2015 [was ")); 
          Serial.print(yr);
          Serial.print(F("]"));
          Serial.println();
          return;
        }
        break;      
      case 1:
        mo = (uint8_t) strtoul(token, NULL, 10);
        if(mo < 1 || mo > 12){
          Serial.print(F("Error: Month must be between 1 and 12 [was ")); 
          Serial.print(mo);
          Serial.print(F("]"));
          Serial.println();
          return;
        }
        break;
      case 2:
        dy = (uint8_t) strtoul(token, NULL, 10);
        if(dy < 1 || dy > 31){
          Serial.print(F("Error: Day must be between 1 and 31 [was ")); 
          Serial.print(dy);
          Serial.print(F("]"));
          Serial.println();
          return;
        }        
        break;
      case 3:
        hr = (uint8_t) strtoul(token, NULL, 10);    
        if(hr > 23){
          Serial.print(F("Error: Hour must be between 0 and 23 [was ")); 
          Serial.print(hr);
          Serial.print(F("]"));
          Serial.println();
          return;
        }        
        break;
      case 4:
        mn = (uint8_t) strtoul(token, NULL, 10);  
        if(mn > 59){
          Serial.print(F("Error: Minute must be between 0 and 59 [was ")); 
          Serial.print(mn);
          Serial.print(F("]"));
          Serial.println();
          return;
        }        
        break;
      case 5:
        sc = (uint8_t) strtoul(token, NULL, 10);      
        if(mn > 59){
          Serial.print(F("Error: Second must be between 0 and 59 [was ")); 
          Serial.print(sc);
          Serial.print(F("]"));
          Serial.println();
          return;
        }          
        break;        
    }
    token = strtok(NULL, ",");
  }
  
  // if we have an RTC set the time in the RTC
  DateTime datetime(yr,mo,dy,hr,mn,sc);
  
  // it's not harmful to do this
  // even if the RTC is not present
  selectSlot3();
  rtc.adjust(datetime);
  
  // also clear the Oscillator Stop Flag
  // this should really be folded into the RTCLib code
  rtcClearOscillatorStopFlag();
  
      
  // at any rate sync the time to this
  setTime(datetime.unixtime());
  
}

void set_mac_address(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }  
  
  uint8_t _mac_address[6] = {0};
  char tmp[32] = {0};
  
  strncpy(tmp, arg, 31); // copy the string so you don't mutilate the argument
  char * token = strtok(tmp, ":");
  uint8_t num_tokens = 0;

  // parse the argument string, expected to be of the form ab:01:33:51:c8:77
  while (token != NULL) {
    if(num_tokens > 5){
      Serial.println(F("Error: Too many octets passed to setmac: "));
      Serial.print(F("       "));
      Serial.println(arg);
      Serial.println();
      return;
    }    
    
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }  
  
  lowercase(arg);
  boolean valid = true;
  if (strncmp(arg, "direct", 6) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_CONNECT_METHOD, CONNECT_METHOD_DIRECT);
    Serial.print(F("Info: Deleteing stored profiles..."));
    if(cc3000.deleteProfiles()){
      Serial.println(F("OK.")); 
    }
    else{
      Serial.println(F("Failed.")); 
    }
  }
  else {
    Serial.print(F("Error: Invalid connection method entered - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    Serial.println(F("       valid options are: 'direct'"));
    valid = false;
  }

  if (valid) {
    recomputeAndStoreConfigChecksum();
  }
}

void set_ssid(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  else if(strncmp("auto", arg, 4) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_SECURITY_MODE, WLAN_SEC_AUTO);
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  boolean valid = true;
  if (strncmp("normal", arg, 6) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_OPERATIONAL_MODE, SUBMODE_NORMAL);
  }
  else if (strncmp("offline", arg, 7) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_OPERATIONAL_MODE, SUBMODE_OFFLINE);
  }
  else {
    Serial.print(F("Error: Invalid operational mode entered - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    Serial.println(F("       valid options are: 'normal', 'offline'"));
    valid = false;
  }

  if(valid) {
    recomputeAndStoreConfigChecksum();
  }
}

void set_temperature_units(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }  
  
  if((strlen(arg) != 1) || ((arg[0] != 'C') && (arg[0] != 'F'))){
    Serial.print(F("Error: temperature unit must be 'C' or 'F', but received '"));
    Serial.print(arg);
    Serial.println(F("'"));
    return;
  }  
  
  eeprom_write_byte((uint8_t *) EEPROM_TEMPERATURE_UNITS, arg[0]);
  recomputeAndStoreConfigChecksum();
}

void set_static_ip_address(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }  
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  const uint8_t noip[4] = {0};
  if (strncmp("dhcp", arg, 4) == 0) {
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
  else if (strncmp("ntp", arg, 3) == 0) {
    eeprom_write_byte((uint8_t *) EEPROM_USE_NTP, 1);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.print(F("Error: Invalid parameter provided to 'use' command - \""));
    Serial.print(arg);
    Serial.println("\"");
    return;
  }
}

void force_command(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  if (strncmp("update", arg, 6) == 0) {    
    Serial.println(F("Info: Erasing last flash page"));
    SUCCESS_MESSAGE_DELAY();                      
    invalidateSignature();    
    configInject("opmode normal\r");
    mode = SUBMODE_NORMAL;
    configInject("exit\r");
  }
  else {
    Serial.print(F("Error: Invalid parameter provided to 'force' command - \""));
    Serial.print(arg);
    Serial.println(F("\""));
    return;
  }  
}

void printDirectory(File dir, int numTabs) {
   for(;;){     
     File entry =  dir.openNextFile();
     if (! entry) {
       // no more files
       break;
     }
     for (uint8_t i=0; i<numTabs; i++) {
       Serial.print(F("\t"));
     }
     char tmp[16] = {0};
     entry.getName(tmp, 16);
     Serial.print(tmp);
     if (entry.isDirectory()) {
       Serial.println(F("/"));
       printDirectory(entry, numTabs+1);
     } else {
       // files have sizes, directories do not
       Serial.print(F("\t"));
       Serial.print(F("\t"));       
       Serial.println(entry.size(), DEC);
     }
     entry.close();
   }
}

void list_command(char * arg){
  if (strncmp("files", arg, 5) == 0){
    if(init_sdcard_ok){
      File root = SD.open("/", FILE_READ);
      printDirectory(root, 0);   
      root.close();       
    }
    else{
      Serial.println(F("Error: SD Card is not initialized, can't list files."));
    }    
  }
  else{
    Serial.print(F("Error: Invalid parameter provided to 'list' command - \""));
    Serial.print(arg);
    Serial.println(F("\""));
  }  
}

void download_one_file(char * filename){
  if(filename != NULL && init_sdcard_ok){    
    File dataFile = SD.open(filename, FILE_READ);
    char last_char_read = NULL;
    if (dataFile) {
      while (dataFile.available()) {
        last_char_read = dataFile.read();
        Serial.write(last_char_read);
      }
      dataFile.close();      
    }
    //else {
    //  Serial.print("Error: Failed to open file named \"");
    //  Serial.print(filename);
    //  Serial.print(F("\""));
    //}    
    if(last_char_read != '\n'){
      Serial.println();        
    }
  }  
}

void crack_datetime_filename(char * filename, uint8_t target_array[4]){
  char temp_str[3] = {0, 0, 0};   
  for(uint8_t ii = 0; ii < 4; ii++){
    strncpy(temp_str, &(filename[ii * 2]), 2);  
    target_array[ii] = atoi(temp_str);
  }

  target_array[0] += 30; // YY is offset from 2000, but epoch time is offset from 1970
}

void make_datetime_filename(uint8_t src_array[4], char * target_filename, uint8_t max_len){
  snprintf(target_filename, max_len, "%02d%02d%02d%02d.csv", 
    src_array[0] - 30, // YY is offset from 2000, but epoch time is offset from 1970
    src_array[1],
    src_array[2],
    src_array[3]);  
}

void advanceByOneHour(uint8_t src_array[4]){

  tmElements_t tm;
  tm.Year   = src_array[0];
  tm.Month  = src_array[1];
  tm.Day    = src_array[2];
  tm.Wday   = 0;
  tm.Hour   = src_array[3];
  tm.Minute = 0;
  tm.Second = 0;
  
  time_t seconds_since_epoch = makeTime(tm);
  seconds_since_epoch += SECS_PER_HOUR; 
  breakTime(seconds_since_epoch, tm);

  src_array[0] = tm.Year;
  src_array[1] = tm.Month;
  src_array[2] = tm.Day;
  src_array[3] = tm.Hour;
}

// does the behavior of executing the one_file_function on a single file
// or on each file in a range of files 
void fileop_command_delegate(char * arg, void (*one_file_function)(char *)){
  char * first_arg = NULL;
  char * second_arg = NULL;
  
  trim_string(arg);
  
  first_arg = strtok(arg, " ");
  second_arg = strtok(NULL, " ");

  if(second_arg == NULL){   
    one_file_function(first_arg);
    }
    else {
    uint8_t cur_date[4] = {0,0,0,0};
    uint8_t end_date[4] = {0,0,0,0};
    crack_datetime_filename(first_arg, cur_date);
    crack_datetime_filename(second_arg, end_date);

    // starting from cur_date, download the file with that name
    char cur_date_filename[16] = {0};
    boolean finished_last_file = false;
    unsigned long previousMillis = millis();
    const long interval = 1000;
    while(!finished_last_file){
      unsigned long currentMillis = millis();
      if(currentMillis - previousMillis >= interval) {        
        previousMillis = currentMillis;   
        petWatchdog();
      }
      memset(cur_date_filename, 0, 16);
      make_datetime_filename(cur_date, cur_date_filename, 15);
      one_file_function(cur_date_filename);
      if(memcmp(cur_date, end_date, 4) == 0){      
        finished_last_file = true;
    }    
      else{
        advanceByOneHour(cur_date);      
  }
    }     
    delayForWatchdog();    
  }  
}

void download_command(char * arg){
  Serial.println(header_row);
  fileop_command_delegate(arg, download_one_file);
  Serial.println("Info: Done downloading.");
}

void delete_one_file(char * filename){
  if(filename != NULL && init_sdcard_ok){   
    if (SD.remove(filename)) {
      Serial.print("Info: Removed file named \"");
      Serial.print(filename);
      Serial.println(F("\""));
    }
//    else {
//      Serial.print("Error: Failed to delete file named \"");
//      Serial.print(filename);
//      Serial.println(F("\""));
//    }    
    }    
  }

void delete_command(char * arg){
  fileop_command_delegate(arg, delete_one_file);
  Serial.println("Info: Done deleting.");
}

void set_mqtt_password(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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

void set_mqtt_topic_prefix(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  // we've reserved 64-bytes of EEPROM for a MQTT prefix
  // so the argument's length must be <= 63
  char prefix[64] = {0};
  uint16_t len = strlen(arg);
  if (len < 64) {
    strncpy(prefix, arg, len);
    eeprom_write_block(prefix, (void *) EEPROM_MQTT_TOPIC_PREFIX, 64);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: MQTT prefix must be less than 64 characters in length"));
  }
}

void topic_suffix_config(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }

  lowercase(arg);
  
  if (strcmp(arg, "enable") == 0){
    eeprom_write_byte((uint8_t *) EEPROM_MQTT_TOPIC_SUFFIX_ENABLED, 1);
    recomputeAndStoreConfigChecksum();
  }
  else if (strcmp(arg, "disable") == 0){
    eeprom_write_byte((uint8_t *) EEPROM_MQTT_TOPIC_SUFFIX_ENABLED, 0);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.print(F("Error: expected 'enable' or 'disable' but got '"));
    Serial.print(arg);
    Serial.println("'");
  }
}


void set_mqtt_server(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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

  static char server[32] = {0};
  memset(server, 0, 32);
  
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  trim_string(arg); // leading and trailing spaces are not relevant 
  uint16_t len = strlen(arg);   
  
  // we've reserved 32-bytes of EEPROM for an update server name
  // so the argument's length must be <= 31      
  if (len < 32) {        
    strncpy(server, arg, 31); // copy the argument as a case-sensitive server name
    lowercase(arg);           // in case it's the "disable" special case, make arg case insensitive
    
    if(strncmp(arg, "disable", 7) == 0){      
      memset(server, 0, 32); // wipe out the update server name 
    }
    
    eeprom_write_block(server, (void *) EEPROM_UPDATE_SERVER_NAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: Update server name must be less than 32 characters in length"));
  }
}

void set_update_filename(char * arg){
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  // we've reserved 32-bytes of EEPROM for an update server name
  // so the argument's length must be <= 31
  char filename[32] = {0};
  uint16_t len = strlen(arg);
  if (len < 32) {
    strncpy(filename, arg, len);
    eeprom_write_block(filename, (void *) EEPROM_UPDATE_FILENAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: Update filename must be less than 32 characters in length"));
  }  
}

void set_ntp_server(char * arg){

  static char server[32] = {0};
  memset(server, 0, 32);
  
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  trim_string(arg); // leading and trailing spaces are not relevant 
  uint16_t len = strlen(arg);   
  
  // we've reserved 32-bytes of EEPROM for an NTP server name
  // so the argument's length must be <= 31      
  if (len < 32) {        
    strncpy(server, arg, 31); // copy the argument as a case-sensitive server name
    lowercase(arg);           // in case it's the "disable" special case, make arg case insensitive
    
    if(strncmp(arg, "disable", 7) == 0){     
      eeprom_write_byte((uint8_t *) EEPROM_USE_NTP, 0); 
      memset(server, 0, 32); // wipe out the NTP server name 
    }
    
    eeprom_write_block(server, (void *) EEPROM_NTP_SERVER_NAME, 32);
    recomputeAndStoreConfigChecksum();
  }
  else {
    Serial.println(F("Error: NTP server name must be less than 32 characters in length"));
  }  
}

void backup(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
  else if (strncmp("so2", arg, 3) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_SO2_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_SO2_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_SO2_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_SO2_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_SO2_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_SO2_CAL_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_SO2_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_SO2_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("o3", arg, 2) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_O3_SENSITIVITY, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_O3_SENSITIVITY, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_O3_CAL_SLOPE, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_O3_CAL_SLOPE, 4);
    eeprom_read_block(tmp, (const void *) EEPROM_O3_CAL_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_O3_CAL_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_O3_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_O3_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }
  else if (strncmp("temp", arg, 4) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_TEMPERATURE_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_TEMPERATURE_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_TEMPERATURE_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_TEMPERATURE_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }  
  else if (strncmp("hum", arg, 3) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_HUMIDITY_OFFSET, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_HUMIDITY_OFFSET, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_HUMIDITY_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_HUMIDITY_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }  
  else if (strncmp("tz", arg, 2) == 0) {
    eeprom_read_block(tmp, (const void *) EEPROM_NTP_TZ_OFFSET_HRS, 4);
    eeprom_write_block(tmp, (void *) EEPROM_BACKUP_NTP_TZ_OFFSET_HRS, 4);

    if (!BIT_IS_CLEARED(backup_check, BACKUP_STATUS_TIMEZONE_CALIBRATION_BIT)) {
      CLEAR_BIT(backup_check, BACKUP_STATUS_TIMEZONE_CALIBRATION_BIT);
      eeprom_write_word((uint16_t *) EEPROM_BACKUP_CHECK, backup_check);
    }
  }    
  else if (strncmp("all", arg, 3) == 0) {
    valid = false;
    configInject("backup mqttpwd\r");
    configInject("backup key\r");
    configInject("backup so2\r");
    configInject("backup o3\r");
    configInject("backup temp\r");
    configInject("backup hum\r");    
    configInject("backup mac\r");
    configInject("backup tz\r");
    Serial.println();
  }
  else {
    valid = false;
    Serial.print(F("Error: Invalid parameter provided to 'backup' command - \""));
    Serial.print(arg);
    Serial.println(F("\""));
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
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }

  // read the value at that address from eeprom
  float current_value = eeprom_read_float((const float *) eeprom_address);  
  
  float value = 0.0;
  if (convertStringToFloat(arg, &value)) {
    if (conversion) {
      value = conversion(value);
    }

    if(current_value != value){
      eeprom_write_float(eeprom_address, value);
      recomputeAndStoreConfigChecksum();
    }
  }
  else {
    Serial.print(F("Error: Failed to convert string \""));
    Serial.print(arg);
    Serial.println(F("\" to decimal number."));
  }
}

void set_ntp_timezone_offset(char * arg){
  set_float_param(arg, (float *) EEPROM_NTP_TZ_OFFSET_HRS, NULL);
}

// convert from nA/ppm to ppb/V
// from SPEC Sensors, Sensor Development Kit, User Manual, Rev. 1.5
// M[V/ppb] = Sensitivity[nA/ppm] * TIA_Gain[kV/A] * 10^-9[A/nA] * 10^3[V/kV] * 10^-3[ppb/ppm]
// TIA_Gain[kV/A] for SO2 = 120
// slope = 1/M
float convert_so2_sensitivity_to_slope(float sensitivity) {
  float ret = 1.0e9f;
  ret /= sensitivity;
  ret /= 120.0f;
  return ret;
}

// sets both sensitivity and slope
void set_so2_sensitivity(char * arg) {
  set_float_param(arg, (float *) EEPROM_SO2_SENSITIVITY, 0);
  set_float_param(arg, (float *) EEPROM_SO2_CAL_SLOPE, convert_so2_sensitivity_to_slope);
}

void set_so2_slope(char * arg) {
  set_float_param(arg, (float *) EEPROM_SO2_CAL_SLOPE, 0);
}

void set_so2_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_SO2_CAL_OFFSET, 0);
}

void set_reported_temperature_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_TEMPERATURE_OFFSET, 0);
}

void set_reported_humidity_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_HUMIDITY_OFFSET, 0);
}

// convert from nA/ppm to ppb/V
// from SPEC Sensors, Sensor Development Kit, User Manual, Rev. 1.5
// M[V/ppb] = Sensitivity[nA/ppm] * TIA_Gain[kV/A] * 10^-9[A/nA] * 10^3[V/kV]  * 10^-3[ppb/ppm]
// TIA_Gain[kV/A] for O3 = 350
// slope = 1/M
float convert_o3_sensitivity_to_slope(float sensitivity) {
  float ret = 1.0e9f;
  ret /= sensitivity;
  ret /= 350.0f;
  return ret;
}

void set_o3_sensitivity(char * arg) {
  set_float_param(arg, (float *) EEPROM_O3_SENSITIVITY, 0);
  set_float_param(arg, (float *) EEPROM_O3_CAL_SLOPE, convert_o3_sensitivity_to_slope);
}

void set_o3_slope(char * arg) {
  set_float_param(arg, (float *) EEPROM_O3_CAL_SLOPE, 0);
}

void set_o3_offset(char * arg) {
  set_float_param(arg, (float *) EEPROM_O3_CAL_OFFSET, 0);
}

void set_private_key(char * arg) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
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
    Serial.println(F("Error: Private key must be exactly 64 characters long, "));
    Serial.print(F("       but was "));
    Serial.print(len);
    Serial.println(F(" characters long."));
  }
}

void recomputeAndStoreConfigChecksum(void) {
  if(!configMemoryUnlocked(__LINE__)){
    return;
  }
  
  uint16_t crc = computeEepromChecksum();
  eeprom_write_word((uint16_t *) EEPROM_CRC_CHECKSUM, crc);
}

uint16_t computeEepromChecksum(void) {
  uint16_t crc = 0;

  // there are EEPROM_CONFIG_MEMORY_SIZE - 2 bytes to compute the CRC16 over
  // the first byte is located at EEPROM_CRC_CHECKSUM + 2
  // the last byte is located at EEPROM_CONFIG_MEMORY_SIZE - 1
  for (uint16_t ii = 0; ii < EEPROM_CONFIG_MEMORY_SIZE - 2; ii++) {
    uint8_t value = eeprom_read_byte((uint8_t *) (EEPROM_CRC_CHECKSUM + 2 + ii));
    crc = _crc16_update(crc, value);
  }
  return crc;
}

uint16_t getStoredEepromChecksum(void){
  return eeprom_read_word((const uint16_t *) EEPROM_CRC_CHECKSUM);  
}

uint16_t computeFlashChecksum(void) {
  uint16_t crc = 0;
  // there are EEPROM_CONFIG_MEMORY_SIZE - 2 bytes to compute the CRC16 over
  // the first byte is located at SECOND_TO_LAST_4K_PAGE_ADDRESS + 2
  // the last byte is located at EEPROM_CONFIG_MEMORY_SIZE - 1  
  for (uint16_t ii = 0; ii < EEPROM_CONFIG_MEMORY_SIZE - 2; ii++) {
    uint8_t value = flash.readByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + 2UL + ((uint32_t) ii));
    crc = _crc16_update(crc, value);
  }
  return crc;
}

uint16_t getStoredFlashChecksum(void){
  uint16_t stored_crc = flash.readByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + 1UL);
  stored_crc <<= 8;
  stored_crc |= flash.readByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + 0UL);
  return stored_crc;
}

/****** GAS SENSOR SUPPORT FUNCTIONS ******/

void selectNoSlot(void) {
  digitalWrite(7, LOW);
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

void selectSlot3(void){
  selectNoSlot();
  digitalWrite(7, HIGH); 
}
/****** LCD SUPPORT FUNCTIONS ******/
void safe_dtostrf(float value, signed char width, unsigned char precision, char * target_buffer, uint16_t target_buffer_length){
  char meta_format_string[16] = "%%.%df";
  char format_string[16] = {0};

  if((target_buffer != NULL) && (target_buffer_length > 0)){  
    snprintf(format_string, 15, meta_format_string, precision); // format string should come out to something like "%.2f"
    snprintf(target_buffer, target_buffer_length - 1, format_string, value);
  }
    
}

void backlightOn(void) {
  uint8_t backlight_behavior = eeprom_read_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP);
  if(backlight_behavior != BACKLIGHT_ALWAYS_OFF){
    digitalWrite(A6, HIGH);
  }
}

void backlightOff(void) {
  uint8_t backlight_behavior = eeprom_read_byte((uint8_t *) EEPROM_BACKLIGHT_STARTUP);
  if(backlight_behavior != BACKLIGHT_ALWAYS_ON){
    digitalWrite(A6, LOW);
  }
}

void lcdFrownie(uint8_t pos_x, uint8_t pos_y){
  if((pos_x < 16) && (pos_y < 2)){
    lcd.setCursor(pos_x, pos_y);
    lcd.write((byte) 1); 
  }
}

void lcdSmiley(uint8_t pos_x, uint8_t pos_y){
  if((pos_x < 16) && (pos_y < 2)){
    lcd.setCursor(pos_x, pos_y);
    lcd.write((byte) 0); 
  }
}

void lcdBars(uint8_t numBars){  
  for(uint8_t ii = 0; ii < numBars && ii < 5; ii++){
    lcd.setCursor(5+ii, 1);
    lcd.write(3); // full bar
  }
  
  if(numBars < 5){
    for(uint8_t ii = 0; ii <  5 - numBars; ii++){
      lcd.setCursor(5 + numBars + ii, 1); 
      lcd.write((byte) 2); // empty bar
    }
  }
}

void setLCD_P(const char * str PROGMEM){  
  char tmp[33] = {0};
  strncpy_P(tmp, str, 32);
  setLCD(tmp);
}

//void dumpDisplayBuffer(void){
//  Serial.print(F("Debug: Line 1: "));
//  for(uint8_t ii = 0; ii < 17; ii++){
//    Serial.print(F("0x"));
//    Serial.print((uint8_t) g_lcd_buffer[0][ii],HEX);
//    if(ii != 16){
//      Serial.print(F(","));
//    }
//  } 
//  Serial.println();
//  Serial.print(F("Debug: Line 2: "));
//  for(uint8_t ii = 0; ii < 17; ii++){
//    Serial.print(F("0x"));
//    Serial.print((uint8_t) g_lcd_buffer[1][ii],HEX);
//    if(ii != 16){
//      Serial.print(F(","));
//    }
//  }
//  Serial.println();  
//}

void repaintLCD(void){
  static char last_painted[2][17] = {
    "                ",
    "                "
  };
  
  //dumpDisplayBuffer();      
  //  if(strlen((char *) &(g_lcd_buffer[0])) <= 16){
  //    lcd.setCursor(0,0);    
  //    lcd.print((char *) &(g_lcd_buffer[0]));
  //  }
  //  
  //  if(strlen((char *) &(g_lcd_buffer[1])) <= 16){
  //    lcd.setCursor(0,1);    
  //    lcd.print((char *) &(g_lcd_buffer[1])); 
  //  }    

  char tmp[2] = " ";
  for(uint8_t line = 0; line < 2; line++){
    for(uint8_t column = 0; column < 16; column++){
      if(last_painted[line][column] != g_lcd_buffer[line][column]){
        tmp[0] = g_lcd_buffer[line][column];
        lcd.setCursor(column, line);
        lcd.print(tmp);
      }
      last_painted[line][column] = g_lcd_buffer[line][column];
    } 
  }  
  
  g_lcd_buffer[0][16] = '\0'; // ensure null termination
  g_lcd_buffer[1][16] = '\0'; // ensure null termination  
}

void setLCD(const char * str){
  clearLCD();
  uint16_t original_length = strlen(str);  
  strncpy((char *) &(g_lcd_buffer[0]), str, 16);  
  if(original_length > 16){   
    strncpy((char *) &(g_lcd_buffer[1]), str + 16, 16);    
  }   
  repaintLCD();
}

void updateLCD(const char * str, uint8_t pos_x, uint8_t pos_y, uint8_t num_chars){
  uint16_t len = strlen(str);
  char * ptr = 0;
  if((pos_y == 0) || (pos_y == 1)){
    ptr = (char *) &(g_lcd_buffer[pos_y]);   
  }
  
  uint8_t x = 0;  // display buffer index
  uint8_t ii = 0; // input string index
  for(x = pos_x, ii = 0;  (x < 16) && (ii < len) && (ii < num_chars); x++, ii++){
    // don't allow the injection of non-printable characters into the display buffer
    if(isprint(str[ii])){    
      ptr[x] = str[ii];
    }
    else{
      ptr[x] = ' ';
    }
  }
  
  repaintLCD();
}

void clearLCD(){
  memset((uint8_t *) &(g_lcd_buffer[0]), ' ', 16);
  memset((uint8_t *) &(g_lcd_buffer[1]), ' ', 16);
  g_lcd_buffer[0][16] = '\0';
  g_lcd_buffer[1][16] = '\0';  
  lcd.clear();
  repaintLCD();
}

boolean index_of(char ch, char * str, uint16_t * index){
  uint16_t len = strlen(str);
  for(uint16_t ii = 0; ii < len; ii++){
    if(str[ii] == ch){
      *index = ii;
      return true;     
    }
  }
  
  return false;
}

void ltrim_string(char * str){
  uint16_t num_leading_spaces = 0;
  uint16_t len = strlen(str);
  for(uint16_t ii = 0; ii < len; ii++){
    if(!isspace(str[ii])){
      break;      
    }     
    num_leading_spaces++;
  }
  
  if(num_leading_spaces > 0){
    // copy the string left, including the null terminator
    // which is why this loop is <= len
    for(uint16_t ii = 0; ii <= len; ii++){
      str[ii] = str[ii + num_leading_spaces];
    }
  }
}

void rtrim_string(char * str){
  // starting at the last character in the string
  // overwrite space characters with null characteres
  // until you reach a non-space character
  // or you overwrite the entire string
  int16_t ii = strlen(str) - 1;  
  while(ii >= 0){
    if(isspace(str[ii])){
      str[ii] = '\0';
  }
    else{
      break;
    }
    ii--;
  }
}

void trim_string(char * str){
  ltrim_string(str);
  
  //Serial.print(F("ltrim: "));
  //Serial.println(str);
  
  rtrim_string(str);
  
  //Serial.print(F("rtrim: "));
  //Serial.println(str);  
}


void replace_nan_with_null(char * str){
  if(strcmp(str, "nan") == 0){    
    strcpy(str, "null");
  }
}

void replace_character(char * str, char find_char, char replace_char){
  uint16_t len = strlen(str);
  for(uint16_t ii = 0; ii < len; ii++){
    if(str[ii] == find_char){
      str[ii] = replace_char;
    }
  }
}

// returns false if truncating the string to the field width
// would result in alter the whole number part of the represented value
// otherwise truncates the string to the field_width and returns true
boolean truncate_float_string(char * str, uint8_t field_width){
  // if there is a decimal point in the string after the field_width character
  // the value doesn't fit on a line at all, let alone after truncation
  // examples for field_width := 3
  //             v-- field boundardy (for field_width := 3)
  // Case 0:  0.3|4  is ok (and will be truncated to 0.3)
  // Case 0b: 0. |   is ok (and will be truncated to 0)    
  // Case 0c: -0.|3  is ok (and will be truncated to 0)            
  // Case 1:  1.2|4  is ok (and will be truncated to 1.2)
  // Case 1b: 1. |   is ok (and will be truncated to 1)
  // Case 1c: 1  |   is ok (and will be truncated to 1)
  // Case 2b: 12.|5  is ok (and will be truncated to 13)  
  // Cas3 2c: 12.|   is ok (and will be truncated to 12)
  // Cas3 2d: 12 |   is ok (and will be truncated to 12)  
  // Case 3:  123|.4 is ok (and will be truncated to 123)
  // Case 3b: 123|.  is ok (and will be truncated to 123)
  // Case 3c: 123|   is ok (and will be truncated to 123)
  // Case 3d: -12|3  is not ok (because it would be truncated to -12)  
  // Case 3f: -12|3. is not ok (because it would be truncated to -12)  
  // Case 4:  123|4  is not ok (because it would be truncated to 123)
  // Case 4b: 123|4. is not ok (because it would be truncated to 123)
  //          012|345678901234567 (index for reference)
  
  uint16_t period_index = 0;

  // first trim the string to remove leading and trailing ' ' characters
  trim_string(str);  

  uint16_t len = strlen(str);
  boolean string_contains_decimal_point = index_of('.', str, &period_index);

  //Serial.print(F("len > field_width: "));
  //Serial.print(len);
  //Serial.print(F(" > "));
  //Serial.print(field_width);
  //Serial.print(F(", string_contains_decimal_point = "));
  //Serial.print(string_contains_decimal_point);
  //Serial.print(F(", period_index = "));
  //Serial.println(period_index);
  
  if(len > field_width){   
    if(string_contains_decimal_point){
      // there's a decimal point in the string somewhere
      // and the string is longer than the field width
      if(period_index > field_width){
        // the decimal point occurs at least 
        // two characters past the field boundary
        return false;
      }
    }
    else{
      // it's a pure whole number
      // and there's not enough room in the field to hold it
      return false;
    }            
  } 
  
  // first truncate the string to the field width if it's longer than the field width
  if(len > field_width){
    str[field_width] = '\0';
    //Serial.print(F("truncated step 1: "));
    //Serial.println(str);
  }
  
  len = strlen(str);
  // if the last character in the string is a decimal point, lop it off
  if((len > 0) && (str[len-1] == '.')){
     str[len-1] = '\0';
     //Serial.print(F("truncated step 2:"));
     //Serial.println(str);
  }    
      
  // it's already adequately truncated if len <= field_width
  return true;
}

// the caller must ensure that there is adequate memory
// allocated to str, so that it can be safely padded 
// to target length
void leftpad_string(char * str, uint16_t target_length){
  uint16_t len = strlen(str);  
  if(len < target_length){
    uint16_t pad_amount = target_length - len;

    // shift the string (including the null temrinator) right by the pad amount
    // by walking backwards from the end to the start
    // and copying characters over (copying the null terminator is why it starts at len)
    for(int16_t ii = len; ii >= 0; ii--){
      str[ii + pad_amount] = str[ii];
    } 
    
    // then put spaces in the front 
    for(uint16_t ii = 0; ii < pad_amount; ii++){
      str[ii] = ' ';
    }
  }
}

void updateLCD(float value, uint8_t pos_x, uint8_t pos_y, uint8_t field_width){
  static char tmp[64] = {0};
  static char asterisks_field[17] = {0};
  memset(tmp, 0, 64);
  memset(asterisks_field, 0, 17);
  
  for(uint8_t ii = 0; (ii < field_width) && (ii < 16); ii++){
     asterisks_field[ii] = '*'; 
  }  
  
  //Serial.print(F("value: "));
  //Serial.println(value,8);  
  
  safe_dtostrf(value, -16, 6, tmp, 16);
  
  //Serial.print(F("dtostrf: "));
  //Serial.println(tmp);
  
  if(!truncate_float_string(tmp, field_width)){
    updateLCD(asterisks_field, pos_x, pos_y, field_width);
    return;
  }
  
  //Serial.print(F("truncate: "));
  //Serial.println(tmp);
  
  leftpad_string(tmp, field_width); 
  //Serial.print(F("leftpad_string: "));
  //Serial.println(tmp);
      
  updateLCD(tmp, pos_x, pos_y, field_width);  
}

void updateLCD(uint32_t ip, uint8_t line_number){
  char tmp[17] = {0};
  snprintf(tmp, 16, "%d.%d.%d.%d", 
    (uint8_t)(ip >> 24),
    (uint8_t)(ip >> 16),
    (uint8_t)(ip >> 8),       
    (uint8_t)(ip >> 0));    
  
  updateLCD(tmp, line_number);
}

void updateLCD(const char * str, uint8_t line_number){
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
void displayRSSI(void){
  char ssid[32] = {0};
  uint32_t index;
  uint8_t valid, rssi, sec;
  uint8_t max_rssi = 0;
  boolean found_ssid = false;
  uint8_t target_network_secMode = 0;
  char ssidname[33]; 
  uint8_t network_security_mode = eeprom_read_byte((const uint8_t *) EEPROM_SECURITY_MODE);   
  eeprom_read_block(ssid, (const void *) EEPROM_SSID, 31);
  
  setLCD_P(PSTR(" SCANNING WI-FI "
                "                "));
  Serial.println(F("Info: Beginning Network Scan..."));                
  SUCCESS_MESSAGE_DELAY();
  if (!cc3000.startSSIDscan(&index)) {
    Serial.println(F("Error: Network Scan Failed"));
    return;
  }
  
  Serial.print(F("Info: Network Scan found "));
  Serial.print(index);
  Serial.println(F(" networks"));
  
  while (index) {
    index--;

    valid = cc3000.getNextSSID(&rssi, &sec, ssidname);
    if(strncmp(ssidname, ssid, 16) == 0){      
      Serial.print(F("Info: Found Access Point \""));
      Serial.print(ssid);
      Serial.print(F("\", "));
      Serial.print(F("RSSI = "));
      Serial.println(rssi);
      found_ssid = true;
      if(rssi > max_rssi){
        max_rssi = rssi; 
        target_network_secMode = sec;
      }
    }
  }
  
  cc3000.stopSSIDscan();
  
  if(found_ssid){
    int8_t rssi_dbm = max_rssi - 128;
    lcdBars(rssi_to_bars(rssi_dbm));
    lcdSmiley(15, 1); // lower right corner
    
    if(network_security_mode == WLAN_SEC_AUTO){
      allowed_to_write_config_eeprom = true;
      if(target_network_secMode == WLAN_SEC_UNSEC){
        set_network_security_mode("open");
      }
      else if(target_network_secMode == WLAN_SEC_WEP){
        set_network_security_mode("wep");
      }
      else if(target_network_secMode == WLAN_SEC_WPA){
        set_network_security_mode("wpa");
      }
      else if(target_network_secMode == WLAN_SEC_WPA2){
        set_network_security_mode("wpa2");
      }
      allowed_to_write_config_eeprom = false;      
    }
    
    ERROR_MESSAGE_DELAY(); // ERROR is intentional here, to get a longer delay
  }
  else{
    updateLCD("NOT FOUND", 1);
    lcdFrownie(15, 1); // lower right corner
    ERROR_MESSAGE_DELAY();
  }
}

uint8_t rssi_to_bars(int8_t rssi_dbm){
  uint8_t num_bars = 0;
  if (rssi_dbm < -87){
    num_bars = 0;
  }
  else if (rssi_dbm < -82){
    num_bars = 1;
  }
  else if (rssi_dbm < -77){
    num_bars = 2;
  }
  else if (rssi_dbm < -72){
    num_bars = 3;
  }
  else if (rssi_dbm < -67){
    num_bars = 4;
  }
  else{
    num_bars = 5;
  }  
  
  return num_bars;
}

boolean restartWifi(){    
  if(!connectedToNetwork()){        
    delayForWatchdog();
    petWatchdog();
    current_millis = millis();
    reconnectToAccessPoint();
    current_millis = millis();
    delayForWatchdog();
    petWatchdog();    
    acquireIpAddress(); 
    current_millis = millis();
    delayForWatchdog();
    petWatchdog();    
    displayConnectionDetails();

    // if (!mdns.begin("airqualityegg", cc3000)) {
    //   Serial.println(F("Error setting up MDNS responder!"));
    //   while(1);     
    // }    
    
    clearLCD();
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
    SUCCESS_MESSAGE_DELAY();  
  
    return true;
  }
}

void reconnectToAccessPoint(void){
  static char ssid[32] = {0};
  static char network_password[32] = {0};
  static uint8_t connect_method = 0;
  static uint8_t network_security_mode = 0;
  static boolean first_access = true;
  
  if(first_access){
    first_access = false;
    connect_method = eeprom_read_byte((const uint8_t *) EEPROM_CONNECT_METHOD);
    network_security_mode = eeprom_read_byte((const uint8_t *) EEPROM_SECURITY_MODE);  
    eeprom_read_block(ssid, (const void *) EEPROM_SSID, 31);
    eeprom_read_block(network_password, (const void *) EEPROM_NETWORK_PWD, 31); 
  }
  
  switch(connect_method){
    case CONNECT_METHOD_DIRECT:
      Serial.print(F("Info: Connecting to Access Point with SSID \""));
      Serial.print(ssid);
      Serial.print(F("\"..."));
      setLCD_P(PSTR("CONNECTING TO AP"));
      updateLCD(ssid, 1);
      delayForWatchdog();
      petWatchdog();
      if(!cc3000.connectToAP(ssid, network_password, network_security_mode)) {
        Serial.print(F("Error: Failed to connect to Access Point with SSID: "));
        Serial.println(ssid);
        Serial.flush();
        updateLCD("FAILED", 1);
        lcdFrownie(15, 1);
        ERROR_MESSAGE_DELAY();
        watchdogForceReset();
      }
      Serial.println(F("OK."));
      updateLCD("CONNECTED", 1);
      lcdSmiley(15, 1);
      SUCCESS_MESSAGE_DELAY();
      break;
    default:
      Serial.println(F("Error: Connection method not currently supported"));
      break;
  }  
}

void acquireIpAddress(void){
  static boolean first_access = true;
  static uint8_t static_ip_address[4] = {0};
  uint8_t noip[4] = {0};

  if(first_access){
    first_access = false;
    eeprom_read_block(static_ip_address, (const void *) EEPROM_STATIC_IP_ADDRESS, 4);
  }
  
  // if it's DHCP we're configured for, engage DHCP process
  if (memcmp(static_ip_address, noip, 4) == 0){
    /* Wait for DHCP to complete */
    Serial.print(F("Info: Request DHCP..."));
    setLCD_P(PSTR(" REQUESTING IP  "));   
    
    const long dhcp_timeout_duration_ms = 60000L;
    unsigned long previous_dhcp_timeout_millis = current_millis;
    uint32_t ii = 0;
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
        ii++;
        if((ii % 5) == 0){    
          petWatchdog();
        }        
      }      

      if(current_millis - previous_progress_dots_millis >= progress_dots_interval){
        previous_progress_dots_millis = current_millis;
        updateLcdProgressDots();
      }     
     
      if(current_millis - previous_dhcp_timeout_millis >= dhcp_timeout_duration_ms){
        Serial.println(F("Error: Failed to acquire IP address via DHCP. Rebooting."));
        Serial.flush();
        updateLCD("FAILED", 1);
        lcdFrownie(15, 1);
        ERROR_MESSAGE_DELAY();    
        watchdogForceReset();
      }      
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

uint32_t arrayToCC3000Ip(uint8_t * ip_array){
  uint32_t ip = 0;
  for(int8_t ii = 3; ii > 0; ii++){
    ip |= ip_array[ii];    
    ip <<= 8;
  }  
  return ip;
}

/****** ADC SUPPORT FUNCTIONS ******/
// returns the measured voltage in Volts
// 62.5 microvolts resolution in 16-bit mode
boolean burstSampleADC(float * result){
  #define NUM_SAMPLES_PER_BURST (8)

  // autoprobe to find out which address the ADC is at in this slot
  static uint8_t viable_addresses[8] = {0};
  static boolean first_access = true;
  if(first_access){
    first_access = false;
    // viable addresses for MCP3421 are 0x68...0x6F
    for(uint8_t ii = 0; ii < 8; ii++){
      viable_addresses[ii] = 0x68 + ii;
    }
  }

  adc.autoprobe(viable_addresses, 8);
  
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
void clearTempBuffers(void){
  memset(converted_value_string, 0, 64);
  memset(compensated_value_string, 0, 64);
  memset(raw_value_string, 0, 64);
  memset(scratch, 0, 512);
  memset(MQTT_TOPIC_STRING, 0, 128);
}

boolean mqttResolve(void){
  uint32_t ip = 0;
  static boolean resolved = false;
  
  char mqtt_server_name[32] = {0};
  if(!resolved){
    eeprom_read_block(mqtt_server_name, (const void *) EEPROM_MQTT_SERVER_NAME, 31);
    setLCD_P(PSTR("   RESOLVING"));
    updateLCD("MQTT SERVER", 1);
    SUCCESS_MESSAGE_DELAY();
    
    if  (!cc3000.getHostByName(mqtt_server_name, &ip) || (ip == 0))  {
      Serial.print(F("Error: Couldn't resolve '"));
      Serial.print(mqtt_server_name);
      Serial.println(F("'"));
      
      updateLCD("FAILED", 1);
      lcdFrownie(15, 1);
      ERROR_MESSAGE_DELAY();
      return false;
    }  
    else{
      resolved = true;
      cc3000IpToArray(ip, mqtt_server_ip);      
      Serial.print(F("Info: Resolved \""));
      Serial.print(mqtt_server_name);
      Serial.print(F("\" to IP address "));
      cc3000.printIPdotsRev(ip);
      
      updateLCD(ip, 1);      
      lcdSmiley(15, 1);
      SUCCESS_MESSAGE_DELAY();          
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
     eeprom_read_block(MQTT_TOPIC_PREFIX, (const void *) EEPROM_MQTT_TOPIC_PREFIX, 63);
     mqtt_suffix_enabled = eeprom_read_byte((const uint8_t *) EEPROM_MQTT_TOPIC_SUFFIX_ENABLED);
     mqtt_auth_enabled = eeprom_read_byte((const uint8_t *) EEPROM_MQTT_AUTH);
     mqtt_port = eeprom_read_dword((const uint32_t *) EEPROM_MQTT_PORT);

     mqtt_client.setServer(mqtt_server_ip, mqtt_port);
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

     clearLCD();         
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

boolean mqttPublish(char * topic, char *str){
  boolean response_status = true;
  
  Serial.print(F("Info: MQTT publishing to topic "));
  Serial.print(topic);
  Serial.print(F("..."));
  
  uint32_t space_required = 5;
  space_required += strlen(topic);
  space_required += strlen(str);
  if(space_required >= 511){
    Serial.println(F("Aborted."));
    response_status = false;
  } 
  else if(mqtt_client.publish(topic, str)){
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
  clearTempBuffers();
  static uint32_t post_counter = 0;  
  uint8_t sample = pgm_read_byte(&heartbeat_waveform[heartbeat_waveform_index++]);
  snprintf(scratch, 511, 
  "{"
  "\"serial-number\":\"%s\","
  "\"converted-value\":%d,"
  "\"firmware-version\":\"%s\","
  "\"publishes\":[\"so2\",\"o3\",\"temperature\",\"humidity\"],"
  "\"counter\":%lu"
  "}", mqtt_client_id, sample, firmware_version, post_counter++);  
  
  if(heartbeat_waveform_index >= NUM_HEARTBEAT_WAVEFORM_SAMPLES){
     heartbeat_waveform_index = 0;
  }

  replace_character(scratch, '\'', '\"');
  
  strcat(MQTT_TOPIC_STRING, MQTT_TOPIC_PREFIX);
  strcat(MQTT_TOPIC_STRING, "heartbeat");    
  if(mqtt_suffix_enabled){
    strcat(MQTT_TOPIC_STRING, "/");      
    strcat(MQTT_TOPIC_STRING, mqtt_client_id);      
  }
  return mqttPublish(MQTT_TOPIC_STRING, scratch);
}

float toFahrenheit(float degC){
  return  (degC * 9.0f / 5.0f) + 32.0f;
}

boolean publishTemperature(){
  clearTempBuffers();
  float temperature_moving_average = calculateAverage(&(sample_buffer[TEMPERATURE_SAMPLE_BUFFER][0]), sample_buffer_depth);
  temperature_degc = temperature_moving_average;
  float raw_temperature = temperature_degc;
  float reported_temperature = temperature_degc - reported_temperature_offset_degC;
  if(temperature_units == 'F'){
    reported_temperature = toFahrenheit(reported_temperature);
    raw_temperature = toFahrenheit(raw_temperature);
  }
  safe_dtostrf(reported_temperature, -6, 2, converted_value_string, 16);
  safe_dtostrf(raw_temperature, -6, 2, raw_value_string, 16);
  
  trim_string(converted_value_string);
  trim_string(raw_value_string);

  replace_nan_with_null(converted_value_string);
  replace_nan_with_null(raw_value_string);
    
  snprintf(scratch, 511,
    "{"
    "\"serial-number\":\"%s\","
    "\"converted-value\":%s,"
    "\"converted-units\":\"deg%c\","
    "\"raw-value\":%s,"
    "\"raw-units\":\"deg%c\","
    "\"sensor-part-number\":\"SHT25\""
    "%s"
    "}", mqtt_client_id, converted_value_string, temperature_units, raw_value_string, temperature_units, gps_mqtt_string);

  replace_character(scratch, '\'', '\"');
    
  strcat(MQTT_TOPIC_STRING, MQTT_TOPIC_PREFIX);
  strcat(MQTT_TOPIC_STRING, "temperature"); 
  if(mqtt_suffix_enabled){
    strcat(MQTT_TOPIC_STRING, "/");      
    strcat(MQTT_TOPIC_STRING, mqtt_client_id);      
  }
       
  return mqttPublish(MQTT_TOPIC_STRING, scratch);
}

boolean publishHumidity(){
  clearTempBuffers();
  float humidity_moving_average = calculateAverage(&(sample_buffer[HUMIDITY_SAMPLE_BUFFER][0]), sample_buffer_depth);
  relative_humidity_percent = humidity_moving_average;
  float raw_humidity = constrain(relative_humidity_percent, 0.0f, 100.0f);
  float reported_humidity = constrain(relative_humidity_percent - reported_humidity_offset_percent, 0.0f, 100.0f);
  
  safe_dtostrf(reported_humidity, -6, 2, converted_value_string, 16);
  safe_dtostrf(raw_humidity, -6, 2, raw_value_string, 16);
  
  trim_string(converted_value_string);
  trim_string(raw_value_string);

  replace_nan_with_null(converted_value_string);
  replace_nan_with_null(raw_value_string);
    
  snprintf(scratch, 511, 
    "{"
    "\"serial-number\":\"%s\","    
    "\"converted-value\":%s,"
    "\"converted-units\":\"percent\","
    "\"raw-value\":%s,"
    "\"raw-units\":\"percent\","  
    "\"sensor-part-number\":\"SHT25\""
    "%s"
    "}", mqtt_client_id, converted_value_string, raw_value_string, gps_mqtt_string);  

  replace_character(scratch, '\'', '\"');

  strcat(MQTT_TOPIC_STRING, MQTT_TOPIC_PREFIX);
  strcat(MQTT_TOPIC_STRING, "humidity");    
  if(mqtt_suffix_enabled){
    strcat(MQTT_TOPIC_STRING, "/");      
    strcat(MQTT_TOPIC_STRING, mqtt_client_id);      
  }  
  return mqttPublish(MQTT_TOPIC_STRING, scratch);
}

void collectTemperature(void){
  float raw_value = 0.0f;
  if(init_sht25_ok){
    if(sht25.getTemperature(&raw_value)){
      addSample(TEMPERATURE_SAMPLE_BUFFER, raw_value);       
      if(sample_buffer_idx == (sample_buffer_depth - 1)){
        temperature_ready = true;
      }
    }
  }
}

void collectHumidity(void){
  float raw_value = 0.0f;
  if(init_sht25_ok){
    if(sht25.getRelativeHumidity(&raw_value)){
      addSample(HUMIDITY_SAMPLE_BUFFER, raw_value);       
      if(sample_buffer_idx == (sample_buffer_depth - 1)){
        humidity_ready = true;
      }
    }
  }
}

// TODO: create a vector to collect a touch sample every half second or so
//       and move all calls to collectTouch out of main processing into vector

void collectTouch(void){
  static uint8_t sample_write_index = 0;
  touch_sample_buffer[sample_write_index++] = touch.capacitiveSensor(30);
  
  if(sample_write_index == TOUCH_SAMPLE_BUFFER_DEPTH){
    sample_write_index = 0; 
  }
}

void processTouchVerbose(boolean verbose_output){
  const uint32_t touch_event_threshold = 85UL;  
  static boolean first_time = true;
  static unsigned long touch_start_millis = 0UL;
  long backlight_interval = 60000L; 
  static boolean backlight_is_on = false;
 
  if(first_time){
    first_time = false;
    backlight_interval = ((long) eeprom_read_word((uint16_t *) EEPROM_BACKLIGHT_DURATION)) * 1000;    
  }
  
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

void advanceSampleBufferIndex(void){
  sample_buffer_idx++;
  if((sample_buffer_idx >= sample_buffer_depth) || (sample_buffer_idx >= MAX_SAMPLE_BUFFER_DEPTH)){
    sample_buffer_idx = 0;
  }
}

void addSample(uint8_t sample_type, float value){
  if((sample_type < 4) && (sample_buffer_idx < MAX_SAMPLE_BUFFER_DEPTH)){
    sample_buffer[sample_type][sample_buffer_idx] = value;    
  }
}

void collectSO2(void){
  float raw_value = 0.0f;
  
  if(init_so2_afe_ok && init_so2_adc_ok){
    selectSlot2();  
    if(burstSampleADC(&raw_value)){      
      addSample(SO2_SAMPLE_BUFFER, raw_value);
      if(sample_buffer_idx == (sample_buffer_depth - 1)){
        so2_ready = true;
      }
    }
  }
    
  selectNoSlot(); 
}

void collectO3(void ){  
  float raw_value = 0.0f;
  
  if(init_o3_afe_ok && init_o3_adc_ok){
    selectSlot1();  
    if(burstSampleADC(&raw_value)){   
      addSample(O3_SAMPLE_BUFFER, raw_value);      
      if(sample_buffer_idx == (sample_buffer_depth - 1)){
        o3_ready = true;
      }      
    }
  }
    
  selectNoSlot();     
}

float pressure_scale_factor(void){
  float ret = 1.0f;
  
  static boolean first_access = true;
  static int16_t altitude_meters = 0.0f;
  
  if(first_access){
    first_access = false;
    altitude_meters = (int16_t) eeprom_read_word((uint16_t *) EEPROM_ALTITUDE_METERS);
  }

  if(altitude_meters != -1){
    // calculate scale factor of altitude and temperature
    const float kelvin_offset = 273.15f;
    const float lapse_rate_kelvin_per_meter = -0.0065f;
    const float pressure_exponentiation_constant = 5.2558774324f;
    
    float outside_temperature_kelvin = kelvin_offset + (temperature_degc - reported_temperature_offset_degC);
    float outside_temperature_kelvin_at_sea_level = outside_temperature_kelvin - lapse_rate_kelvin_per_meter * altitude_meters; // lapse rate is negative
    float pow_arg = 1.0f + ((lapse_rate_kelvin_per_meter * altitude_meters) / outside_temperature_kelvin_at_sea_level);
    ret = powf(pow_arg, pressure_exponentiation_constant);
  }
  
  return ret;
}

void so2_convert_from_volts_to_ppb(float volts, float * converted_value, float * temperature_compensated_value){
  static boolean first_access = true;
  static float so2_zero_volts = 0.0f;
  static float so2_slope_ppb_per_volt = 0.0f;
  float temperature_coefficient_of_span = 0.0f;
  float temperatureresmpensated_slope = 0.0f;
  if(first_access){
    // SO2 has positive slope in circuit, more positive voltages correspond to higher levels of SO2
    so2_slope_ppb_per_volt = eeprom_read_float((const float *) EEPROM_SO2_CAL_SLOPE); 
    so2_zero_volts = eeprom_read_float((const float *) EEPROM_SO2_CAL_OFFSET);
    first_access = false;
  }

  // apply piecewise linear regressions
  // to signal scaling effect curve
  float scaling_slope = 0.0f;
  float scaling_intercept = 0.0f;  
  if(temperature_degc < 0.0f){                 // < 0C  
    scaling_slope = 1.2251676453f;
    scaling_intercept = 78.1252902858f;
  }
  else if(temperature_degc < 20.0f){           // 0C .. 20C   
    scaling_slope = 1.0484934163f;
    scaling_intercept = 78.4862696133f; 
  }
  else{                                        // > 20C   
    scaling_slope = 0.6255065526f;
    scaling_intercept = 87.1964488084f;
  }
  float signal_scaling_factor_at_temperature = ((scaling_slope * temperature_degc) + scaling_intercept)/100.0f;
  // divide by 100 becauset the slope/intercept graphs have scaling factors in value of "%"

  // apply piecewise linear regressions
  // to baseline offset effect curve
  float baseline_offset_ppm_slope = 0.0f;
  float baseline_offset_ppm_intercept = 0.0f;
                                                                     
  if(temperature_degc < 5.0f){                          // < 5C
    baseline_offset_ppm_slope = 0.0096303567f;
    baseline_offset_ppm_intercept = 0.1451019634f;
  }
  else if(temperature_degc < 16.5f){                     // 5C .. 16.5C
    baseline_offset_ppm_slope = 0.107934957f;
    baseline_offset_ppm_intercept = -0.4613725299f;
  }
  else if(temperature_degc < 23.5f){                     // 16.5C .. 23.5C
    baseline_offset_ppm_slope = 0.2160747618f;
    baseline_offset_ppm_intercept = -2.186942354f;    
  }
  else if(temperature_degc < 32.0f){                     // 23.5C .. 32C
    baseline_offset_ppm_slope = 0.3723989228f;
    baseline_offset_ppm_intercept = -5.8469088214f;          
  }
  else{                                                  // > 32C
    baseline_offset_ppm_slope = 0.5913110128f;
    baseline_offset_ppm_intercept =  -13.0513308725f;        
  }  
  float baseline_offset_ppm_at_temperature = ((baseline_offset_ppm_slope * temperature_degc) + baseline_offset_ppm_intercept); 
  float baseline_offset_ppb_at_temperature = baseline_offset_ppm_at_temperature * 1000.0f;
  // multiply by 1000 because baseline offset graph shows SO2 in ppm  
  float baseline_offset_voltage_at_temperature = baseline_offset_ppb_at_temperature / so2_slope_ppb_per_volt;

  float signal_scaling_factor_at_altitude = pressure_scale_factor();

  *converted_value = (volts - so2_zero_volts) * so2_slope_ppb_per_volt;
  if(*converted_value <= 0.0f){
    *converted_value = 0.0f; 
  }
  
  *temperature_compensated_value = (volts - so2_zero_volts - baseline_offset_voltage_at_temperature) * so2_slope_ppb_per_volt 
                                   / signal_scaling_factor_at_temperature 
                                   / signal_scaling_factor_at_altitude;
                                   
  if(*temperature_compensated_value <= 0.0f){
    *temperature_compensated_value = 0.0f;
  }
}

boolean publishSO2(){
  clearTempBuffers();
  float converted_value = 0.0f, compensated_value = 0.0f;    
  float so2_moving_average = calculateAverage(&(sample_buffer[SO2_SAMPLE_BUFFER][0]), sample_buffer_depth);
  so2_convert_from_volts_to_ppb(so2_moving_average, &converted_value, &compensated_value);
  so2_ppb = compensated_value;  
  safe_dtostrf(so2_moving_average, -8, 6, raw_value_string, 16);
  safe_dtostrf(converted_value, -4, 2, converted_value_string, 16);
  safe_dtostrf(compensated_value, -4, 2, compensated_value_string, 16); 
  
  trim_string(raw_value_string);
  trim_string(converted_value_string);
  trim_string(compensated_value_string);  

  replace_nan_with_null(raw_value_string);
  replace_nan_with_null(converted_value_string);
  replace_nan_with_null(compensated_value_string);
  
  snprintf(scratch, 511, 
    "{"
    "\"serial-number\":\"%s\","       
    "\"raw-value\":%s,"
    "\"raw-units\":\"volt\","
    "\"converted-value\":%s,"
    "\"converted-units\":\"ppb\","
    "\"compensated-value\":%s,"
    "\"sensor-part-number\":\"3SP-SO2-20-PCB\""
    "%s"
    "}",
    mqtt_client_id,
    raw_value_string, 
    converted_value_string, 
    compensated_value_string,
    gps_mqtt_string);  

  replace_character(scratch, '\'', '\"'); // replace single quotes with double quotes
  
  strcat(MQTT_TOPIC_STRING, MQTT_TOPIC_PREFIX);
  strcat(MQTT_TOPIC_STRING, "so2");    
  if(mqtt_suffix_enabled){
    strcat(MQTT_TOPIC_STRING, "/");      
    strcat(MQTT_TOPIC_STRING, mqtt_client_id);      
  }  
  return mqttPublish(MQTT_TOPIC_STRING, scratch);      
}

void o3_convert_from_volts_to_ppb(float volts, float * converted_value, float * temperature_compensated_value){
  static boolean first_access = true;
  static float o3_zero_volts = 0.0f;
  static float o3_slope_ppb_per_volt = 0.0f;
  float temperature_coefficient_of_span = 0.0f;
  float temperature_compensated_slope = 0.0f;  
  if(first_access){
    // O3 has positive slope in circuit, more positive voltages correspond to higher levels of O3
    o3_slope_ppb_per_volt = eeprom_read_float((const float *) EEPROM_O3_CAL_SLOPE);
    o3_zero_volts = eeprom_read_float((const float *) EEPROM_O3_CAL_OFFSET);
    first_access = false;
  }

  // apply piecewise linear regressions
  // to signal scaling effect curve
  float scaling_slope = 0.0f;
  float scaling_intercept = 0.0f;  
  if(temperature_degc < 0.0f){                 // < 0C  
    scaling_slope = -0.0355739076f;
    scaling_intercept = 97.9865525718f;
  }
  else if(temperature_degc < 20.0f){           // 0C .. 20C   
    scaling_slope = 0.1702484721f;
    scaling_intercept = 97.9953985672f; 
  }
  else{                                        // > 20C   
    scaling_slope = 0.3385634354f;
    scaling_intercept = 94.6638669473f;
  }
  float signal_scaling_factor_at_temperature = ((scaling_slope * temperature_degc) + scaling_intercept)/100.0f;
  // divide by 100 becauset the slope/intercept graphs have scaling factors in value

  // apply piecewise linear regressions
  // to baseline offset effect curve
  float baseline_offset_ppm_slope = 0.0f;
  float baseline_offset_ppm_intercept = 0.0f;
                                                                     
  if(temperature_degc < 33.0f){                          // < 33C
    baseline_offset_ppm_slope = -0.0007019288f;
    baseline_offset_ppm_intercept = 0.0177058403f;
  }
  else if(temperature_degc < 38.0f){                     // 33C .. 38C
    baseline_offset_ppm_slope = -0.0085978946f;
    baseline_offset_ppm_intercept = 0.2777254052f;
  }
  else if(temperature_degc < 42.0f){                     // 38C .. 42C
    baseline_offset_ppm_slope = -0.0196092331f;
    baseline_offset_ppm_intercept = 0.6994563331f;    
  }
  else if(temperature_degc < 46.0f){                     // 42C .. 46C
    baseline_offset_ppm_slope = -0.0351416006f;
    baseline_offset_ppm_intercept = 1.3566041809f;          
  }
  else{                                                  // > 46C
    baseline_offset_ppm_slope = -0.0531894279f;
    baseline_offset_ppm_intercept =  2.1948987152f;        
  }  
  float baseline_offset_ppm_at_temperature = (baseline_offset_ppm_slope * temperature_degc) + baseline_offset_ppm_intercept;  
  float baseline_offset_ppb_at_temperature = baseline_offset_ppm_at_temperature * 1000.0f;
  // multiply by 1000 because baseline offset graph shows O3 in ppm  
  float baseline_offset_voltage_at_temperature = -1.0f * baseline_offset_ppb_at_temperature / o3_slope_ppb_per_volt;
  // multiply by -1 because the ppm curve goes negative but the voltage actually *increases*

  float signal_scaling_factor_at_altitude = pressure_scale_factor();
    
  *converted_value = (o3_zero_volts - volts) * o3_slope_ppb_per_volt;
  if(*converted_value <= 0.0f){
    *converted_value = 0.0f; 
  }


  *temperature_compensated_value = (o3_zero_volts - volts - baseline_offset_voltage_at_temperature) * o3_slope_ppb_per_volt 
                                   / signal_scaling_factor_at_temperature
                                   / signal_scaling_factor_at_altitude;
                                   
  if(*temperature_compensated_value <= 0.0f){
    *temperature_compensated_value = 0.0f;
  }
}

boolean publishO3(){
  clearTempBuffers();  
  float converted_value = 0.0f, compensated_value = 0.0f;   
  float o3_moving_average = calculateAverage(&(sample_buffer[O3_SAMPLE_BUFFER][0]), sample_buffer_depth);
  o3_convert_from_volts_to_ppb(o3_moving_average, &converted_value, &compensated_value);
  o3_ppb = compensated_value;  
  safe_dtostrf(o3_moving_average, -8, 6, raw_value_string, 16);
  safe_dtostrf(converted_value, -4, 2, converted_value_string, 16);
  safe_dtostrf(compensated_value, -4, 2, compensated_value_string, 16);  
    
  trim_string(raw_value_string);
  trim_string(converted_value_string);
  trim_string(compensated_value_string);    

  replace_nan_with_null(raw_value_string);
  replace_nan_with_null(converted_value_string);
  replace_nan_with_null(compensated_value_string);
  
  snprintf(scratch, 511, 
    "{"
    "\"serial-number\":\"%s\","      
    "\"raw-value\":%s,"
    "\"raw-units\":\"volt\","
    "\"converted-value\":%s,"
    "\"converted-units\":\"ppb\","
    "\"compensated-value\":%s,"
    "\"sensor-part-number\":\"3SP-O3-20-PCB\""
    "%s"
    "}",
    mqtt_client_id,
    raw_value_string, 
    converted_value_string, 
    compensated_value_string,
    gps_mqtt_string);  

  replace_character(scratch, '\'', '\"'); // replace single quotes with double quotes

  strcat(MQTT_TOPIC_STRING, MQTT_TOPIC_PREFIX);
  strcat(MQTT_TOPIC_STRING, "o3");    
  if(mqtt_suffix_enabled){
    strcat(MQTT_TOPIC_STRING, "/");      
    strcat(MQTT_TOPIC_STRING, mqtt_client_id);      
  }  
  return mqttPublish(MQTT_TOPIC_STRING, scratch);      
}

void petWatchdog(void){
  tinywdt.pet(); 
}

void delayForWatchdog(void){
  delay(120); 
}

void watchdogForceReset(void){
  tinywdt.force_reset(); 
  Serial.println(F("Error: Watchdog Force Restart failed. Manual reset is required."));
  setLCD_P(PSTR("AUTORESET FAILED"
                " RESET REQUIRED "));                
  backlightOn();                
  ERROR_MESSAGE_DELAY();
  for(;;){
    delay(1000);
  }
}

void watchdogInitialize(void){
  tinywdt.begin(100, 65000); 
}

// modal operation loop functions
void loop_wifi_mqtt_mode(void){
  static uint8_t num_mqtt_connect_retries = 0;
  static uint8_t num_mqtt_intervals_without_wifi = 0; 
  
  // mqtt publish timer intervals
  static unsigned long previous_mqtt_publish_millis = 0;
  
  if(current_millis - previous_mqtt_publish_millis >= reporting_interval){   
    previous_mqtt_publish_millis = current_millis;      
    
    printCsvDataLine();
    
    if(connectedToNetwork()){
      num_mqtt_intervals_without_wifi = 0;
      
      if(mqttReconnect()){         
        updateLCD("TEMP ", 0, 0, 5);
        updateLCD("RH ", 10, 0, 3);         
        updateLCD("SO2 ", 0, 1, 4);
        updateLCD("O3 ", 10, 1, 3);
                      
        //connected to MQTT server and connected to Wi-Fi network        
        num_mqtt_connect_retries = 0;   
        if(!publishHeartbeat()){
          Serial.println(F("Error: Failed to publish Heartbeat."));  
        }
        
        if(init_sht25_ok){
          if(temperature_ready){
            if(!publishTemperature()){          
              Serial.println(F("Error: Failed to publish Temperature."));          
            }
            else{
              float reported_temperature = temperature_degc - reported_temperature_offset_degC;
              if(temperature_units == 'F'){
                reported_temperature = toFahrenheit(reported_temperature);
              }
              updateLCD(reported_temperature, 5, 0, 3);             
            }
          }
          else{
            updateLCD("---", 5, 0, 3);
          }        
        }
        else{
          // sht25 is not ok
          updateLCD("XXX", 5, 0, 3);
        }
        
        if(init_sht25_ok){
          if(humidity_ready){
            if(!publishHumidity()){
              Serial.println(F("Error: Failed to publish Humidity."));         
            }
            else{
              float reported_relative_humidity_percent = relative_humidity_percent - reported_humidity_offset_percent;
              updateLCD(reported_relative_humidity_percent, 13, 0, 3);  
            }
          }
          else{
            updateLCD("---", 13, 0, 3);
          }
        }
        else{
          updateLCD("XXX", 13, 0, 3);
        }
        
        if(init_so2_afe_ok && init_so2_adc_ok){
          if(so2_ready){
            if(!publishSO2()){
              Serial.println(F("Error: Failed to publish SO2."));          
            }
            else{
              updateLCD(so2_ppb, 5, 1, 3);  
            }
          }
          else{
            updateLCD("---", 5, 1, 3); 
          }
        }
        else{
          updateLCD("XXX", 5, 1, 3); 
        }
        
        if(init_o3_afe_ok && init_o3_adc_ok){
          if(o3_ready){
            if(!publishO3()){
              Serial.println(F("Error: Failed to publish O3."));         
            }
            else{
              updateLCD(o3_ppb, 13, 1, 3); 
            }
          }
          else{
            updateLCD("---", 13, 1, 3);  
          }
        }
        else{
          updateLCD("XXX", 13, 1, 3);
        }
    
      }
      else{
        // not connected to MQTT server
        num_mqtt_connect_retries++;
        Serial.print(F("Warn: Failed to connect to MQTT server "));
        Serial.print(num_mqtt_connect_retries);
        Serial.print(F(" consecutive time"));
        if(num_mqtt_connect_retries > 1){
          Serial.print(F("s"));           
        }
        Serial.println();
        
        if(num_mqtt_connect_retries >= 5){
          Serial.println(F("Error: MQTT Connect Failed 5 consecutive times. Forcing reboot."));
          Serial.flush();
          setLCD_P(PSTR("  MQTT SERVER   "
                        "    FAILURE     "));
          lcdFrownie(15, 1);
          ERROR_MESSAGE_DELAY();            
          watchdogForceReset();  
        }
      }
    }
    else{
      // not connected to Wi-Fi network
      num_mqtt_intervals_without_wifi++;
      Serial.print(F("Warn: Failed to connect to Wi-Fi network "));
      Serial.print(num_mqtt_intervals_without_wifi);
      Serial.print(F(" consecutive time"));
      if(num_mqtt_intervals_without_wifi > 1){
        Serial.print(F("s"));
      }
      Serial.println();      
      if(num_mqtt_intervals_without_wifi >= 5){
        Serial.println(F("Error: Wi-Fi Re-connect Failed 5 consecutive times. Forcing reboot."));
        Serial.flush();
        setLCD_P(PSTR(" WI-FI NETWORK  "
                      "    FAILURE     "));
        lcdFrownie(15, 1);
        ERROR_MESSAGE_DELAY();         
        watchdogForceReset();  
      }
      
      restartWifi();
    }
  }    
}

void loop_offline_mode(void){
  
  // write record timer intervals
  static unsigned long previous_write_record_millis = 0;

  if(current_millis - previous_write_record_millis >= reporting_interval){   
    previous_write_record_millis = current_millis;
    printCsvDataLine();
  }  
}

/****** SIGNAL PROCESSING MATH SUPPORT FUNCTIONS ******/

float calculateAverage(float * buf, uint16_t num_samples){
  float average = 0.0f;
  for(uint16_t ii = 0; ii < num_samples; ii++){
    average += buf[ii];
  } 
  
  return average / num_samples;
}

void printCsvDataLine(){
  static boolean first = true;
  static char dataString[512] = {0};  
  memset(dataString, 0, 512);
  
  uint16_t len = 0;
  uint16_t dataStringRemaining = 511;
  
  if(first){
    first = false;      
    Serial.print(F("csv: "));    
    Serial.print(header_row);
    Serial.println();
  }  
  
  Serial.print(F("csv: "));
  printCurrentTimestamp(dataString, &dataStringRemaining);
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  if(temperature_ready){
    temperature_degc = calculateAverage(&(sample_buffer[TEMPERATURE_SAMPLE_BUFFER][0]), sample_buffer_depth);
    float reported_temperature = temperature_degc - reported_temperature_offset_degC;
    if(temperature_units == 'F'){
      reported_temperature = toFahrenheit(reported_temperature);
    }
    Serial.print(reported_temperature, 2);
    appendToString(reported_temperature, 2, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  if(humidity_ready){
    relative_humidity_percent = calculateAverage(&(sample_buffer[HUMIDITY_SAMPLE_BUFFER][0]), sample_buffer_depth);
    float reported_relative_humidity = relative_humidity_percent - reported_humidity_offset_percent;        
    Serial.print(reported_relative_humidity, 2);
    appendToString(reported_relative_humidity, 2, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }    
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  float so2_moving_average = 0.0f;
  if(so2_ready){
    float converted_value = 0.0f, compensated_value = 0.0f;    
    so2_moving_average = calculateAverage(&(sample_buffer[SO2_SAMPLE_BUFFER][0]), sample_buffer_depth);
    so2_convert_from_volts_to_ppb(so2_moving_average, &converted_value, &compensated_value);
    so2_ppb = compensated_value;      
    Serial.print(so2_ppb, 2);
    appendToString(so2_ppb, 2, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  float o3_moving_average = 0.0f;
  if(o3_ready){    
    float converted_value = 0.0f, compensated_value = 0.0f;   
    o3_moving_average = calculateAverage(&(sample_buffer[O3_SAMPLE_BUFFER][0]), sample_buffer_depth);
    o3_convert_from_volts_to_ppb(o3_moving_average, &converted_value, &compensated_value);
    o3_ppb = compensated_value;     
    Serial.print(o3_ppb, 2);
    appendToString(o3_ppb, 2, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }   
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  Serial.print(so2_moving_average, 6);
  appendToString(so2_moving_average, 6, dataString, &dataStringRemaining);
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);
  
  
  Serial.print(o3_moving_average, 6);
  appendToString(o3_moving_average, 6, dataString, &dataStringRemaining);
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);

  if(gps_latitude != TinyGPS::GPS_INVALID_F_ANGLE){
    Serial.print(gps_latitude, 6);
    appendToString(gps_latitude, 6, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }
  
  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);

  if(gps_longitude != TinyGPS::GPS_INVALID_F_ANGLE){
    Serial.print(gps_longitude, 6);
    appendToString(gps_longitude, 6, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }  

  Serial.print(F(","));
  appendToString("," , dataString, &dataStringRemaining);

  if(gps_altitude != TinyGPS::GPS_INVALID_F_ALTITUDE){
    Serial.print(gps_altitude, 6);
    appendToString(gps_altitude, 2, dataString, &dataStringRemaining);
  }
  else{
    Serial.print(F("---"));
    appendToString("---", dataString, &dataStringRemaining);
  }  


  Serial.println();
  appendToString("\n", dataString, &dataStringRemaining);  
  
  if((mode == SUBMODE_OFFLINE) && init_sdcard_ok){
    char filename[16] = {0};
    getNowFilename(filename, 15);     
    File dataFile = SD.open(filename, FILE_WRITE);
    if (dataFile) {
      dataFile.print(dataString);
      dataFile.close();
      setLCD_P(PSTR("  LOGGING DATA  "
                    "   TO SD CARD   "));   
    }
    else {
      Serial.print("Error: Failed to open SD card file named \"");
      Serial.print(filename);
      Serial.println(F("\""));
      setLCD_P(PSTR("  SD CARD FILE  "
                    "  OPEN FAILED   "));
      lcdFrownie(15, 1);      
    }
  }
  else if((mode == SUBMODE_OFFLINE) && !init_sdcard_ok){
    setLCD_P(PSTR("  LOGGING DATA  "
                  "  TO USB-SERIAL "));
  }
}

boolean mode_requires_wifi(uint8_t opmode){
  boolean requires_wifi = false;
  
  if(opmode == SUBMODE_NORMAL){
    requires_wifi = true;
  } 
  
  return requires_wifi;
}

/****** INITIALIZATION SUPPORT FUNCTIONS ******/

// the following defines are what goes in the SPI flash where to signal to the bootloader
#define LAST_4K_PAGE_ADDRESS      0x7F000     // the start address of the last 4k page
#define MAGIC_NUMBER              0x00ddba11  // this word at the end of SPI flash
                                              // is a signal to the bootloader to 
                                              // think about loading it
#define MAGIC_NUMBER_ADDRESS      0x7FFFC     // the last 4 bytes are the magic number
#define CRC16_CHECKSUM_ADDRESS    0x7FFFA     // the two bytes before the magic number
                                              // are the expected checksum of the file
#define FILESIZE_ADDRESS          0x7FFF6     // the four bytes before the checksum
                                              // are the stored file size

#define IDLE_TIMEOUT_MS  10000     // Amount of time to wait (in milliseconds) with no data 
                                   // received before closing the connection.  If you know the server
                                   // you're accessing is quick to respond, you can reduce this value.

void invalidateSignature(void){
  flash_file_size = 0;
  flash_signature = 0;  
  while(flash.busy()){;}   
  flash.blockErase4K(LAST_4K_PAGE_ADDRESS);
  while(flash.busy()){;}   
}

// returns the number of header bytes in the server response
// if the file was downloaded in one chunk, this means that
// mybuffer[ret] is the first byte of the response body
uint16_t downloadFile(char * filename, void (*responseBodyProcessor)(uint8_t, boolean, unsigned long, uint16_t)){    
  uint16_t ret = 0;

  // re-initialize the globals
  unsigned long total_bytes_read = 0;
  unsigned long body_bytes_read = 0;
  uint16_t crc16_checksum = 0;
  uint8_t mybuffer[512] = {0};
  
  /* Try connecting to the website.
     Note: HTTP/1.1 protocol is used to keep the server from closing the connection before all data is read.
  */
  WildFire_CC3000_Client www = cc3000.connectTCP(update_server_ip32, 80);
  if (www.connected()) {
    www.fastrprint(F("GET /"));
    www.fastrprint(filename);
    www.fastrprint(F(" HTTP/1.1\r\n"));
    www.fastrprint(F("Host: ")); www.fastrprint(update_server_name); www.fastrprint(F("\r\n"));
    www.fastrprint(F("\r\n"));
    www.println();
  } else {
    Serial.println(F("Error: Update Server Connection failed"));    
    return 0;
  }

  Serial.println(F("Info: -------------------------------------"));
  
  /* Read data until either the connection is closed, or the idle timeout is reached. */ 
  unsigned long lastRead = millis();
  unsigned long num_chunks = 0;
  unsigned long num_bytes_read = 0;
  unsigned long num_header_bytes = 0;
  unsigned long start_time = millis();
  
  #define PARSING_WAITING_FOR_CR       0
  #define PARSING_WAITING_FOR_CRNL     1
  #define PARSING_WAITING_FOR_CRNLCR   2
  #define PARSING_WAITING_FOR_CRNLCRNL 3  
  #define PARSING_FOUND_CRNLCRNL       4
  uint8_t parsing_state = PARSING_WAITING_FOR_CR;
  // get past the response headers    
  while (www.connected() && (millis() - lastRead < IDLE_TIMEOUT_MS)) {   
    while (www.available()) {
      //char c = www.read();
      num_bytes_read = www.read(mybuffer, 255);
      num_chunks++;
      
      if((num_chunks % 20) == 0){
        petWatchdog();
        updateCornerDot();
      }
      
      for(uint32_t ii = 0 ; ii < num_bytes_read; ii++){
         if(parsing_state != PARSING_FOUND_CRNLCRNL){
           num_header_bytes++;
         }
         
         switch(parsing_state){
         case PARSING_WAITING_FOR_CR:
           if(mybuffer[ii] == '\r'){
             parsing_state = PARSING_WAITING_FOR_CRNL;
           }
           break;
         case PARSING_WAITING_FOR_CRNL:
           if(mybuffer[ii] == '\n'){
             parsing_state = PARSING_WAITING_FOR_CRNLCR;
           }         
           else{
             parsing_state = PARSING_WAITING_FOR_CR;
           }
           break;
         case PARSING_WAITING_FOR_CRNLCR:
           if(mybuffer[ii] == '\r'){
             parsing_state = PARSING_WAITING_FOR_CRNLCRNL;
           }         
           else{
             parsing_state = PARSING_WAITING_FOR_CR;
           }         
           break;
         case PARSING_WAITING_FOR_CRNLCRNL:
           if(mybuffer[ii] == '\n'){
             parsing_state = PARSING_FOUND_CRNLCRNL;
           }         
           else{
             parsing_state = PARSING_WAITING_FOR_CR;
           }         
           break;             
         default:           
           crc16_checksum = _crc16_update(crc16_checksum, mybuffer[ii]);
           if(responseBodyProcessor != 0){
             responseBodyProcessor(mybuffer[ii], false, body_bytes_read, crc16_checksum);
             body_bytes_read++;
           }
           break;
         }               
      }
      //Serial.println(num_bytes_read);
      total_bytes_read += num_bytes_read;
      uint16_t address = 0;
      uint8_t data_byte = 0;       
      lastRead = millis();
    }
  }
  
  www.close();
  
  if(responseBodyProcessor != 0){
    responseBodyProcessor(0, true, body_bytes_read, crc16_checksum); // signal end of stream
  }  
  
  unsigned long end_time = millis();
  Serial.println(F("Info: -------------------------------------"));
  Serial.print("Info: # Bytes Read: ");
  Serial.println(total_bytes_read);
  Serial.print("Info: # Chunks Read: ");
  Serial.println(num_chunks);
  Serial.print("Info: File Size: ");
  Serial.println(total_bytes_read - num_header_bytes);
  Serial.print("Info: CRC16 Checksum: ");
  Serial.println(crc16_checksum);
  Serial.print("Info: Download Time: ");
  Serial.println(end_time - start_time); 
  
  return num_header_bytes;
}

void checkForFirmwareUpdates(){ 
  static char filename[64] = {0};
  memset(filename, 0, 64);
  
  if(updateServerResolve()){
    // try and download the integrity check file up to three times    
    setLCD_P(PSTR("  CHECKING FOR  "
                  "    UPDATES     "));
    eeprom_read_block(filename, (const void *) EEPROM_UPDATE_FILENAME, 31);
    strncat_P(filename, PSTR(".chk"), 4);    
    uint16_t num_hdr_bytes = 0;
    for(uint8_t ii = 0; ii < 3; ii++){
      Serial.print(F("Info: Attempt #"));
      Serial.print(ii+1);
      Serial.print(F(" downloading \""));
      Serial.print(filename);
      Serial.print(F("\""));
      Serial.println();
      
      num_hdr_bytes = downloadFile(filename, processIntegrityCheckBody);   
      if(downloaded_integrity_file){
        lcdSmiley(15, 1);
        SUCCESS_MESSAGE_DELAY();
        delayForWatchdog();        
        petWatchdog();        
        break; 
      }
    }

    if(downloaded_integrity_file){
      // compare the just-retrieved signature file contents 
      // to the signature already stored in flash
      if((flash_file_size != integrity_num_bytes_total) || 
        (flash_signature != integrity_crc16_checksum)){                 
        
        setLCD_P(PSTR("UPDATE AVAILABLE"
                      "  DOWNLOADING   "));       
        SUCCESS_MESSAGE_DELAY(); 
        delayForWatchdog();        
        petWatchdog();
       
        
        memset(filename, 0, 64); // switch to the hex extension
        eeprom_read_block(filename, (const void *) EEPROM_UPDATE_FILENAME, 31);
        strncat_P(filename, PSTR(".hex"), 4); 
        
        setLCD_P(PSTR("     PLEASE     "
                      "      WAIT      "));  
                      
        Serial.print(F("Info: Downloading \""));
        Serial.print(filename);
        Serial.print(F("\""));
        Serial.println();
        
        downloadFile(filename, processUpdateHexBody);    
        while(flash.busy()){;}           
        if(integrity_check_succeeded){ 
          // also write these parameters to their rightful place in the SPI flash
          // for consumption by the bootloader
          invalidateSignature();                                 
          
          flash.writeByte(CRC16_CHECKSUM_ADDRESS + 0, (integrity_crc16_checksum >> 8) & 0xff);
          flash.writeByte(CRC16_CHECKSUM_ADDRESS + 1, (integrity_crc16_checksum >> 0) & 0xff);
          
          flash.writeByte(FILESIZE_ADDRESS + 0, (integrity_num_bytes_total >> 24) & 0xff);
          flash.writeByte(FILESIZE_ADDRESS + 1, (integrity_num_bytes_total >> 16) & 0xff);    
          flash.writeByte(FILESIZE_ADDRESS + 2, (integrity_num_bytes_total >> 8)  & 0xff);
          flash.writeByte(FILESIZE_ADDRESS + 3, (integrity_num_bytes_total >> 0)  & 0xff);                
          
          flash.writeByte(MAGIC_NUMBER_ADDRESS + 0, MAGIC_NUMBER >> 24); 
          flash.writeByte(MAGIC_NUMBER_ADDRESS + 1, MAGIC_NUMBER >> 16); 
          flash.writeByte(MAGIC_NUMBER_ADDRESS + 2, MAGIC_NUMBER >> 8); 
          flash.writeByte(MAGIC_NUMBER_ADDRESS + 3, MAGIC_NUMBER >> 0); 
          Serial.println(F("Info: Wrote Magic Number"));          
          
          Serial.println(F("Info: Firmware Update Complete. Reseting to apply changes."));
          setLCD_P(PSTR("APPLYING UPDATES"
                        "WAIT ONE MINUTE "));          
          lcdSmiley(15, 1);
          SUCCESS_MESSAGE_DELAY();
          watchdogForceReset();        
        }
        else{
          Serial.println(F("Error: Firmware Update Failed. Try again later by resetting."));
          setLCD_P(PSTR(" UPDATE FAILED  "
                        "  RETRY LATER   "));
          lcdFrownie(15, 1);
          ERROR_MESSAGE_DELAY();         
        }
      }
      else{
        Serial.println("Info: Signature matches, skipping HEX download.");
        setLCD_P(PSTR("SOFTWARE ALREADY"
                      "   UP TO DATE   "));       
        SUCCESS_MESSAGE_DELAY();   
        delayForWatchdog();        
        petWatchdog();        
      }
    }
    else{
      Serial.println("Error: Failed to download integrity check file, skipping Hex file download");
    }    
  }  
}

boolean updateServerResolve(void){
  static boolean resolved = false;
  
  if(connectedToNetwork()){ 
    if(!resolved){
      eeprom_read_block(update_server_name, (const void *) EEPROM_UPDATE_SERVER_NAME, 31);      

      if(strlen(update_server_name) == 0){
        return false; // this is as indication that OTA updates are disabled
      }
      
      setLCD_P(PSTR("   RESOLVING"));
      updateLCD("UPDATE SERVER", 1);
      SUCCESS_MESSAGE_DELAY();
      
      if  (!cc3000.getHostByName(update_server_name, &update_server_ip32) || (update_server_ip32 == 0)){
        Serial.print(F("Error: Couldn't resolve '"));
        Serial.print(update_server_name);
        Serial.println(F("'"));
        
        updateLCD("FAILED", 1);
        lcdFrownie(15, 1);
        ERROR_MESSAGE_DELAY();
        return false;
      }
      else{
        resolved = true;    
        Serial.print(F("Info: Resolved \""));
        Serial.print(update_server_name);
        Serial.print(F("\" to IP address "));
        cc3000.printIPdotsRev(update_server_ip32);
        
        updateLCD(update_server_ip32, 1);      
        lcdSmiley(15, 1);
        SUCCESS_MESSAGE_DELAY();          
        Serial.println();    
      }
    }
     
    // connected to network and resolution succeeded 
    return true;
  }
  
  // not connected to network
  return false;
}

void processIntegrityCheckBody(uint8_t dataByte, boolean end_of_stream, unsigned long body_bytes_read, uint16_t crc16_checksum){
  char * endPtr;
  static char buff[64] = {0};
  static uint8_t buff_idx = 0;  
  
  if(end_of_stream){
    integrity_num_bytes_total = strtoul(buff, &endPtr, 10);
    if(endPtr != 0){
      integrity_crc16_checksum = strtoul(endPtr, 0, 10);
      downloaded_integrity_file = true;
    }
    Serial.println("Info: Integrity Checks: ");
    Serial.print(  "Info:    File Size: ");
    Serial.println(integrity_num_bytes_total);
    Serial.print(  "Info:    CRC16 Checksum: ");
    Serial.println(integrity_crc16_checksum);                
  }
  else{
    if(buff_idx < 63){
      buff[buff_idx++] = dataByte;
    }
  }
}

void processUpdateHexBody(uint8_t dataByte, boolean end_of_stream, unsigned long body_bytes_read, uint16_t crc16_checksum){
  static uint8_t page[256] = {0};
  static uint16_t page_idx = 0;
  static uint32_t page_address = 0;
  
  if(page_idx < 256){
    page[page_idx++] = dataByte;  
    if(page_idx >= 256){
       page_idx = 0;
    }
  }
  
  if(end_of_stream || (page_idx == 0)){
    if((page_address % 4096) == 0){
      while(flash.busy()){;}    
      flash.blockErase4K(page_address); 
      while(flash.busy()){;}   
    }    
    
    uint16_t top_bound = 256;
    if(page_idx != 0){
      top_bound = page_idx;
    }
    flash.writeBytes(page_address, page, top_bound);
    
    
    // clear the page
    memset(page, 0, 256);
    
    // advance the page address
    page_address += 256;
    
  }
  
  if(end_of_stream){
    if((body_bytes_read == integrity_num_bytes_total) && (crc16_checksum == integrity_crc16_checksum)){
      integrity_check_succeeded = true;
      Serial.println(F("Info: Integrity Check Succeeded!"));
    }
    else{
      Serial.println(F("Error: Integrity Check Failed!"));
      Serial.print(F("Error: Expected Checksum: "));
      Serial.print(integrity_crc16_checksum);
      Serial.print(F(", Actual Checksum: "));
      Serial.println(crc16_checksum);
      Serial.print(F("Error: Expected Filesize: "));
      Serial.print(integrity_num_bytes_total);
      Serial.print(F(", Actual Filesize: "));
      Serial.println(body_bytes_read);
    }
  }
}

void getCurrentFirmwareSignature(void){  
  // retrieve the current signature parameters  
  flash_file_size = flash.readByte(FILESIZE_ADDRESS);
  flash_file_size <<= 8;
  flash_file_size |= flash.readByte(FILESIZE_ADDRESS+1);
  flash_file_size <<= 8;
  flash_file_size |= flash.readByte(FILESIZE_ADDRESS+2);  
  flash_file_size <<= 8;
  flash_file_size |= flash.readByte(FILESIZE_ADDRESS+3);  

  flash_signature = flash.readByte(CRC16_CHECKSUM_ADDRESS);
  flash_signature <<= 8;
  flash_signature |= flash.readByte(CRC16_CHECKSUM_ADDRESS+1);

  Serial.print(F("Info: Current firmware signature: "));
  Serial.print(flash_file_size);
  Serial.print(F(" "));
  Serial.print(flash_signature);
  Serial.println();   
}

/****** CONFIGURATION MIRRORING SUPPORT FUNCTIONS ******/
void commitConfigToMirroredConfig(void){
  if(!mirrored_config_matches_eeprom_config()){      
    mirrored_config_copy_from_eeprom(); // create a valid mirrored config from the current settings             
    if(!mirrored_config_integrity_check()){
      Serial.println(F("Error: Mirrored configuration commit failed to validate.")); 
      //TODO: should something be written to the LCD here?
    }
  }
  else{
    Serial.println(F("Info: Mirrored configuration already matches current configuration.")); 
  } 
}

boolean mirrored_config_matches_eeprom_config(void){
  boolean ret = true;

  // compare each corresponding byte of the Flash into the EEPROM
  for (uint16_t ii = 0; ii < EEPROM_CONFIG_MEMORY_SIZE; ii++) {
    uint8_t flash_value = flash.readByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + ((uint32_t) ii));
    uint8_t eeprom_value = eeprom_read_byte((uint8_t *) (EEPROM_CRC_CHECKSUM + ii));
    if(flash_value != eeprom_value){
      ret = false;
      break;
    }
  }
  
  return ret;
} 

boolean configMemoryUnlocked(uint16_t call_id){
  if(!allowed_to_write_config_eeprom){
    Serial.print(F("Error: Config Memory is not unlocked, called from line number "));
    Serial.println(call_id);
    return false; 
  }
  
  return allowed_to_write_config_eeprom;
}

boolean mirrored_config_integrity_check(){
  boolean ret = false;
  uint16_t computed_crc = computeFlashChecksum();
  
  // interpret the CRC, little endian
  uint16_t stored_crc = getStoredFlashChecksum();
  
  if(stored_crc == computed_crc){
    ret = true; 
  }
  
  return ret;  
}


void mirrored_config_restore(void){  
  if(!allowed_to_write_config_eeprom){
    return;
  }

  // copy each byte from the Flash into the EEPROM
  for (uint16_t ii = 0; ii < EEPROM_CONFIG_MEMORY_SIZE; ii++) {
    uint8_t value = flash.readByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + ((uint32_t) ii));
    eeprom_write_byte((uint8_t *) (EEPROM_CRC_CHECKSUM + ii), value);
  }
}

boolean mirrored_config_restore_and_validate(void){
  boolean integrity_check_passed = false;
  
  if(mirrored_config_integrity_check()){
    mirrored_config_restore();
    integrity_check_passed = checkConfigIntegrity();
    if(integrity_check_passed){
      Serial.println(F("Info: Successfully restored to last valid configuration.")); 
    }
    else{
      Serial.println(F("Info: Restored last valid configuration, but it's still not valid."));
    }  
  }
  else{
    Serial.println(F("Error: Mirrored configuration is not valid, cannot restore to last valid configuration.")); 
  }
  
  return integrity_check_passed;
}

void mirrored_config_copy_from_eeprom(void){
  
  mirrored_config_erase();
  Serial.print(F("Info: Writing mirrored config..."));

  // copy each byte from the EEPROM into the Flash
  for (uint16_t ii = 0; ii < EEPROM_CONFIG_MEMORY_SIZE; ii++) {
    uint8_t value = eeprom_read_byte((uint8_t *) (EEPROM_CRC_CHECKSUM + ii));
    flash.writeByte(((uint32_t) SECOND_TO_LAST_4K_PAGE_ADDRESS) + ((uint32_t) ii), value);    
  }
  Serial.println(F("OK."));
}

void mirrored_config_erase(void){
  Serial.print(F("Info: Erasing mirrored config..."));  
  flash.blockErase4K(SECOND_TO_LAST_4K_PAGE_ADDRESS);
  Serial.println(F("OK."));  
}

/****** TIMESTAMPING SUPPORT FUNCTIONS ******/
time_t AQE_now(void){
  selectSlot3();
  DateTime t = rtc.now();
  return (time_t) t.unixtime();
}

void currentTimestamp(char * dst, uint16_t max_len){
  time_t n = now();
  
  snprintf(dst, max_len, "%02d/%02d/%04d %02d:%02d:%02d", 
    month(n),
    day(n),
    year(n),
    hour(n),
    minute(n),
    second(n));
}

void printCurrentTimestamp(char * append_to, uint16_t * append_to_capacity_and_update){
  char datetime[32] = {0};
  currentTimestamp(datetime, 31);
  Serial.print(datetime);
  
  appendToString(datetime, append_to, append_to_capacity_and_update);  
}

void appendToString(char * str, char * append_to, uint16_t * append_to_capacity_and_update){
  if(append_to != 0){
    uint16_t len = strlen(str);
    if(*append_to_capacity_and_update >= len){
      strcat(append_to, str);
      *append_to_capacity_and_update -= len;
    }
  }  
}

void appendToString(float val, uint8_t digits_after_decimal_point, char * append_to, uint16_t * append_to_capacity_and_update){
  char temp[32] = {0};
  safe_dtostrf(val, 0, digits_after_decimal_point, temp, 31);
  appendToString(temp, append_to, append_to_capacity_and_update);
}

void getNowFilename(char * dst, uint16_t max_len){
  time_t n = now();
  snprintf(dst, max_len, "%02d%02d%02d%02d.csv", 
    year(n) % 100,
    month(n),
    day(n),
    hour(n));
}

void rtcClearOscillatorStopFlag(void){
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write(DS3231_REG_CONTROL);
    Wire.endTransmission();

    // control registers
    Wire.requestFrom(DS3231_ADDRESS, 2);
    uint8_t creg = Wire.read(); 
    uint8_t sreg = Wire.read();   
    
    sreg &= ~_BV(7); // clear bit 7 (msbit)
    
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write((uint8_t) DS3231_REG_STATUS_CTL);
    Wire.write((uint8_t) sreg);
    Wire.endTransmission();    
}

/****** GPS SUPPORT FUNCTIONS ******/
void updateGpsStrings(void){
  const char * gps_lat_lng_field_mqtt_template = ",\"latitude\":%10.6f,\"longitude\":%11.6f";
  const char * gps_lat_lng_alt_field_mqtt_template  = ",\"latitude\":%10.6f,\"longitude\":%11.6f,\"altitude\":%8.2f"; 
  const char * gps_lat_lng_field_csv_template = ",%10.6f,%11.6f,---";
  const char * gps_lat_lng_alt_field_csv_template  = ",%10.6f,%11.6f,%8.2f";   
  
  memset(gps_mqtt_string, 0, GPS_MQTT_STRING_LENGTH);
  memset(gps_csv_string, 0, GPS_CSV_STRING_LENGTH);
  strcpy_P(gps_csv_string, PSTR(",---,---,---"));
  
  if((gps_latitude != TinyGPS::GPS_INVALID_F_ANGLE) && (gps_longitude != TinyGPS::GPS_INVALID_F_ANGLE)){
    if(gps_altitude != TinyGPS::GPS_INVALID_F_ALTITUDE){
      snprintf(gps_mqtt_string, GPS_MQTT_STRING_LENGTH-1, gps_lat_lng_alt_field_mqtt_template, gps_latitude, gps_longitude, gps_altitude);
      snprintf(gps_csv_string, GPS_CSV_STRING_LENGTH-1, gps_lat_lng_alt_field_csv_template, gps_latitude, gps_longitude, gps_altitude);
    }
    else{
      snprintf(gps_mqtt_string, GPS_MQTT_STRING_LENGTH-1, gps_lat_lng_field_mqtt_template, gps_latitude, gps_longitude);
      snprintf(gps_csv_string, GPS_CSV_STRING_LENGTH-1, gps_lat_lng_field_csv_template, gps_latitude, gps_longitude);
    }    
  }
}

/****** NTP SUPPORT FUNCTIONS ******/
void getNetworkTime(void){
  const unsigned long connectTimeout  = 15L * 1000L; // Max time to wait for server connection
  const unsigned long responseTimeout = 15L * 1000L; // Max time to wait for data from server  
  char server[32] = {0};  
  eeprom_read_block(server, (void *) EEPROM_NTP_SERVER_NAME, 31);
  uint8_t       buf[48];
  unsigned long ip, startTime, t = 0L;
  
  if(cc3000.getHostByName(server, &ip)) {
    static const char PROGMEM
      timeReqA[] = { 227,  0,  6, 236 },
      timeReqB[] = {  49, 78, 49,  52 };
    
    Serial.print(F("Info: Getting NTP Time..."));
    startTime = millis();
    do {
      ntpClient = cc3000.connectUDP(ip, 123);
    } while((!ntpClient.connected()) &&
            ((millis() - startTime) < connectTimeout));

    if(ntpClient.connected()) {      
      // Assemble and issue request packet
      memset(buf, 0, sizeof(buf));
      memcpy_P( buf    , timeReqA, sizeof(timeReqA));
      memcpy_P(&buf[12], timeReqB, sizeof(timeReqB));
      ntpClient.write(buf, sizeof(buf));
      memset(buf, 0, sizeof(buf));
      startTime = millis();
      while((!ntpClient.available()) &&
            ((millis() - startTime) < responseTimeout));
      if(ntpClient.available()) {
        ntpClient.read(buf, sizeof(buf));
        t = (((unsigned long)buf[40] << 24) |
             ((unsigned long)buf[41] << 16) |
             ((unsigned long)buf[42] <<  8) |
              (unsigned long)buf[43]) - 2208988800UL;      
      }
      ntpClient.close();
    }
  }

  if(t){
    t += eeprom_read_float((float *) EEPROM_NTP_TZ_OFFSET_HRS) * 60UL * 60UL; // convert offset to seconds
    tmElements_t tm;
    breakTime(t, tm);
    setTime(t);

    selectSlot3();     
    DateTime datetime(t);
    rtc.adjust(datetime);
    rtcClearOscillatorStopFlag();
    selectNoSlot();

    memset(buf, 0, 48);
    snprintf((char *) buf, 47, 
      "%d/%d/%d",
      tm.Month,
      tm.Day,
      1970 + tm.Year);
      
    clearLCD();
    updateLCD((char *) buf, 0);    
    
    Serial.print((char *) buf);
    Serial.print(" ");
    memset(buf, 0, 48);
    snprintf((char *) buf, 47, 
      "%02d:%02d:%02d",
      tm.Hour,
      tm.Minute,
      tm.Second);

    updateLCD((char *) buf, 1);
    Serial.println((char *) buf);

    
    SUCCESS_MESSAGE_DELAY(); 
    
  }
  else{
    Serial.print(F("Failed."));
  }
}

/*
void dump_config(uint8_t * buf){    
  uint16_t addr = 0;
  uint8_t ii = 0;
  while(addr < EEPROM_CONFIG_MEMORY_SIZE){
    if(ii == 0){
      Serial.print(addr, HEX);
      Serial.print(F(": ")); 
    }    
    
    Serial.print(buf[addr], HEX);
    Serial.print(F("\t"));
    
    addr++;
    ii++;
    if(ii == 32){
      Serial.println();
      ii = 0; 
    }
  }
}
*/
