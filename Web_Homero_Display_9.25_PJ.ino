#include <SparkFunBME280.h>
#include "Adafruit_Si7021.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
//#include <ButtonDebounce.h>
// Include NTPclient libraries
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

#define MAXSENSOR         2
#define SENSOR_IDLE       0
#define SENSOR_CONNECTING 1
#define SENSOR_CONNECTED  2
#define SENSOR_HASDATA    3
#define SENSOR_DATAREAD   4

// May have to be changed at daylaight savings
// Deli Osztrak timezone
#define TIMEZONE        10
// Pulus timezone
//#define TIMEZONE        1
// 10 percenkent frissitjuk az idot az NTP serverrol
#define NTP_REFRESH     600000

// D5-ös láb
#define BACKLIGHT_PIN 14

// tavoli szenszor frissites (ms)
#define REMOTE_UPDATE 300000

// Global vars
const char* ssid1 = "xxx";
const char* password1 = "xxx";

const char* ssid2 = "xxx";
const char* password2 = "xxx";

const char* ntp_pool = "au.pool.ntp.org";
//const char* ntp_pool = "europe.pool.ntp.org";

String tspapiKey = "xxx";
int tspfield = 1;
float tempadjust = -1.0;
float humadjust = 0.0;
float pressadjust = 0.0;
float tempfirstsample = 0.0;

int villogas = 1;
int debug = 0;

uint8_t sensorid = 1;

int i2caddr_bme280 = 0x76;
int fake_bme = 0;       // Si7021 alias fake BME280, ScanI2C will adjust this

// Legfontosabb sor!
LiquidCrystal_I2C lcd(0x3F, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);

// Normal vars
const int led = BUILTIN_LED;
unsigned long currentmilis, prevmilis;
unsigned long last_tsp_milis, last_blink_milis, last_rootload_milis, last_lcdupdate_milis, last_lcd_time_update_millis, last_backlight_on_millis;

int init_bme280 = 255; // 255 sikertelen, 0 sikeres
int ledstatus = 0;

// NTP client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntp_pool, TIMEZONE*3600, NTP_REFRESH);
time_t now;
struct tm *now2;

struct sensordata
{
  String hostname;
  WiFiClient client;
  int status;
  int id;
  float t, tmin, tmax, p, h;
  long lastconnectedmillis;
  String ssid;
  long rssi; //signal strength
  String datastring;
} sensor[MAXSENSOR];

// melyik mezot mutassuk a t mellett
// Ezt a valtozot mindig megnoveljük és modulo 4 
// nezzuk mit irjunk ki a T melle
int display_field=0;

WiFiClient client;
BME280 mySensor;
Adafruit_Si7021 myFakeSensor; // = Adafruit_Si7021();
ESP8266WebServer server(80);

WiFiEventHandler stationDisconnectedHandler;
String wifi_event = "Wifi Event: "; // Ha van wifi disconnect event, akkor ebbe beletesszük
String buildinfo = "";

//   Van benne EEPROM
//   Jo otletnek tunt hasznalni
//   EEPROM layout
//   
struct webhomero_eeprom
{
  uint8_t isinited;   // jelezük, hogy inicializáltuk-e
  uint8_t sensorid;   // sensorid
  uint8_t tspfield; // A tinhgspeak-en ez az első mező ahonnan írunk, egy csatornára több hőmérő is dolgozik
  uint8_t villogas;    // villogas ki vagy bekapcsolása
  float tempadjust;       // legfontosabb mező, mennyivel módosítjuk a mért érékeket

  int length_tspapiKey;
  String tspapiKey;
  int length_wifi1_ssid;
  String wifi1_ssid;
  int length_wifi1_passwd;
  String wifi1_passwd;
} myeprom;

// 24 merest tarolunk, hogy tudjuk mikor volt a nap
// leghidegebb és legmelegebb tagja
struct meres
{
  // mikor nyitjuk ki ezt az intervallumot
  // akkor kell lépni, ha 3600*1000 ms eltelt
  long millis;

  float tmax;
  long  tmaxtime;
  float tmin;
  long  tmintime;

} templast24[24];
// Ez mutatja hol járunk  a fenti tömbben.
int last24curpos = 0;


// Ez a függvény kiírja a program nevét
String display_Running_Sketch (void) {
  String the_path = __FILE__;
  int slash_loc = the_path.lastIndexOf('\\');
  String the_cpp_name = the_path.substring(slash_loc + 1);
  int dot_loc = the_cpp_name.lastIndexOf('.');
  String the_sketchname = the_cpp_name.substring(0, dot_loc);

  Serial.print("\nArduino is running Sketch: ");
  Serial.println(the_sketchname);
  Serial.print("Compiled on: ");
  Serial.print(__DATE__);
  Serial.print(" at ");
  Serial.print(__TIME__);
  Serial.print("\n");

  the_sketchname += " (Compiled on: ";
  the_sketchname += __DATE__;
  the_sketchname += " ";
  the_sketchname += __TIME__;
  the_sketchname += " )";

  return (the_sketchname);
}

// Ez a függvény a globális EEPROM objektummal dolgozik
// kiolvassa belőle az értékeket és beleteszi a globalis myeprom
// struktúrába
boolean eeprom_read()
{
  int i = 0;
  float f;
  uint8_t u;

  //read isinitited
  myeprom.isinited = EEPROM.read(i);
  i += sizeof(uint8_t);
  Serial.print("\nmyeprom.isinited: "); Serial.print(myeprom.isinited);

  // read sensorid ;
  myeprom.sensorid = EEPROM.read(i);
  i += sizeof(uint8_t);
  Serial.print("\nmyeprom.sensorid: "); Serial.print(myeprom.sensorid);

  //read startfield
  myeprom.tspfield = EEPROM.read(i);
  i += sizeof(uint8_t);
  Serial.print("\nmyeprom.startfield: "); Serial.print(myeprom.tspfield);

  //read villogas
  myeprom.villogas = EEPROM.read(i);
  i += sizeof(uint8_t);
  Serial.print("\nmyeprom.villogj: "); Serial.print(myeprom.villogas);

  //read tempadjust
  EEPROM.get(i, f);
  myeprom.tempadjust = f;
  i += sizeof(float);
  Serial.print("\nmyeprom.tempadjust: "); Serial.print(myeprom.tempadjust);

  if ( myeprom.isinited == 1) return true;
  return false;
}

// Ez a függvény a globális EEPROM objektummal dolgozik
// Beírja és commitálja a globalis myeprom
// struktúrából az értékeket
boolean eeprom_write()
{
  int i = 0;
  float f;
  uint8_t u;

  //write isinited
  EEPROM.write(i, 1);
  i += sizeof(uint8_t);
  Serial.print("\nWriting myeprom.isinited: "); Serial.print(1);

  // write sensorid
  EEPROM.write(i, myeprom.sensorid);
  i += sizeof(uint8_t);
  Serial.print("\nWriting myeprom.sesnsorid: "); Serial.print(sensorid);

  //write startfield
  u = myeprom.tspfield;
  EEPROM.write(i, u);
  i += sizeof(uint8_t);
  Serial.print("\nWriting myeprom.startfield: "); Serial.print(u);

  //write villogas
  u = myeprom.villogas;
  EEPROM.write(i, u);
  i += sizeof(uint8_t);
  Serial.print("\nWriting myeprom.villogas: "); Serial.print(u);

  //read adjust
  f = myeprom.tempadjust;
  EEPROM.put(i, f);
  i += sizeof(float);
  Serial.print("\nWriting myeprom.tempadjust: "); Serial.print(f);

  Serial.print("\nEEPROM commit... ");
  EEPROM.commit();

  Serial.print("\nReading back: ");
  eeprom_read();
}

// Kiirja, hogy hol vannak a csatlakoztatott
// I2C eszközök
void scan_I2Cbus()
{
  // Scan the I2C bus, return the address of the first device
  byte error, address;
  int nDevices;

  Serial.println("Scanning I2C bus...");

  nDevices = 0;
  for (address = 1; address < 127; address++ )
  {
    // The i2c_scanner uses the return value of
    // the Write.endTransmisstion to see if
    // a device did acknowledge to the address.
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0)
    {
      Serial.print("I2C device found at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.print(address, HEX);
      Serial.print("  !\n");
      if (address == 0x40) {    // Fake BME280 found!!! Si7021 has 2 addresses, 0x40 is one of them (0x76 the other)
          fake_bme = 1;
          Serial.print("Fake BME280 (Si7021) found at address: 0x");
          Serial.println(address, HEX);
      }
      nDevices++;
    }
    else if (error == 4)
    {
      Serial.print("Unknown error at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No I2C devices found\n");
  else
    Serial.println("done\n");
}



int setup_bme280(byte address)
{
  bool initres;
  float t;
  //***Driver settings********************************//
  //commInterface can be I2C_MODE or SPI_MODE
  //specify chipSelectPin using arduino pin names
  //specify I2C address.  Can be 0x77(default) or 0x76

  //For I2C, enable the following and disable the SPI section
  //mySensor.settings.commInterface = I2C_MODE;
  // mySensor.settings.I2CAddress = 0x76;
  // mySensor.settings.I2CAddress = address;


  mySensor.reset();
  //mySensor.settings.commInterface = I2C_MODE;
  mySensor.setI2CAddress(address);

  //For SPI enable the following and dissable the I2C section
  //mySensor.settings.commInterface = SPI_MODE;
  //mySensor.settings.chipSelectPin = 10;


  //***Operation settings*****************************//

  //runMode can be:
  //  0, Sleep mode
  //  1 or 2, Forced mode
  //  3, Normal mode
  mySensor.setMode (MODE_FORCED); //sleep mode

  //tStandby can be:
  //  0, 0.5ms
  //  1, 62.5ms
  //  2, 125ms
  //  3, 250ms
  //  4, 500ms
  //  5, 1000ms
  //  6, 10ms
  //  7, 20ms
  mySensor.setStandbyTime(3);

  //filter can be off or number of FIR coefficients to use:
  //  0, filter off
  //  1, coefficients = 2
  //  2, coefficients = 4
  //  3, coefficients = 8
  //  4, coefficients = 16
  mySensor.setFilter(0);

  //tempOverSample can be:
  //  0, skipped
  //  1 through 5, oversampling *1, *2, *4, *8, *16 respectively
  mySensor.setTempOverSample(3);

  //pressOverSample can be:
  //  0, skipped
  //  1 through 5, oversampling *1, *2, *4, *8, *16 respectively
  mySensor.setPressureOverSample(1);

  //humidOverSample can be:
  //  0, skipped
  //  1 through 5, oversampling *1, *2, *4, *8, *16 respectively
  mySensor.setHumidityOverSample(1);

  delay(50);  //Make sure sensor had enough time to turn on. BME280 requires 2ms to start up.         Serial.begin(57600);


  Serial.print("\nStarting BME280 at address 0x");
  Serial.print(address);
  //Calling .begin() causes the settings to be loaded
  initres = mySensor.beginI2C();
  mySensor.setMode(MODE_SLEEP);

  if ( initres == true )
  {
    Serial.print(": SUCCESS");
    init_bme280 = 0;
  }
  else
  {
    Serial.print(": FAILED");
  }
  t = readadjustedtemp();
  tempfirstsample = t;
  Serial.print("\nsetup_bme280: Temp: ");
  Serial.print(t);
  Serial.print("\nsetup_bme280: Humidity: ");
  Serial.print(mySensor.readFloatHumidity());
  Serial.print("\nsetup_bme280: Pressure: ");
  Serial.print(mySensor.readFloatPressure());
  Serial.print("\nsetup_bme280: finished\n");
  return init_bme280;
}


int setup_fake_bme280()
{
  float t;

  myFakeSensor = Adafruit_Si7021();
  Serial.print("\nStarting FAKE BME280 (Si7021) at address 0x76 and 0x40");

  if (!myFakeSensor.begin()) {
    Serial.println(": FAILED. Did not find Si7021 sensor!");
    init_bme280 = 255;
  } else {
    Serial.print(": SUCCESS");
    init_bme280 = 0;
    t = readadjustedtemp();
    tempfirstsample = t;
    Serial.print("\nsetup_fake_bme280: Temp: ");
    Serial.print(t);
    Serial.print("\nsetup_fake_bme280: Humidity: ");
    Serial.print(myFakeSensor.readHumidity());
    Serial.print("\nsetup_fake_bme280: Pressure: HAHAHA. Si7021 doesn't do pressure!");
    Serial.print("\nsetup_fake_bme280: finished\n");
  }
     
  return init_bme280;
}



// forced módban működő szenzorral mér
// igazított értéket ad vissza
float readadjustedtemp()
{
  float t = 0.0;

  if (fake_bme) {
    t = myFakeSensor.readTemperature();
    
  } else {
    mySensor.setMode(MODE_FORCED); //Wake up sensor and take reading
    while (mySensor.isMeasuring() == false) ; //Wait for sensor to start measurment
    while (mySensor.isMeasuring() == true) ; //Hang out while sensor completes the reading

    t = mySensor.readTempC();
  }
  Serial.print("\nreadadjustedtemp: Sensor reports:");
  Serial.print( t );
  Serial.print(" adjusting to ");
  Serial.print(t + tempadjust);
  Serial.print(" NTPtime: ");
  timeClient.update();
  Serial.print(timeClient.getFormattedTime());

  return ( t + tempadjust );
}

float readadjustedhum()
{
  float h = 0.0;

  if (fake_bme) {
    h = myFakeSensor.readHumidity();    
  } else {
    h = mySensor.readFloatHumidity();
  }
    return ( h + humadjust );
}


float readadjustedpress()
{
  float p = 0.0;

  if (fake_bme) {
    p = 0;  
  } else {
    p = mySensor.readFloatPressure();
  }
  return ( p + pressadjust );
}



// Log to ThingSpeak
// debug módban, vagy ha üres a tspapiKey 
// nem tolt fel adatot
void logtsp()
{

  float t, h, p;
  String str_f1 = "field";
  String str_f2 = "field";
  String str_f3 = "field";

  currentmilis = millis();

  t = readadjustedtemp();
  h = readadjustedhum();
  p = readadjustedpress();
  
  str_f1 += (String)tspfield;
  str_f2 += (String)(tspfield + 1);
  str_f3 += (String)(tspfield + 2);

  if ( debug == 1) return;
  if ( tspapiKey == "" ) 
    {
      Serial.print("\n Skipping thingspeak  data upload.");
      return;
    }

  if (client.connect("api.thingspeak.com", 80)) { //   "184.106.153.149" or api.thingspeak.com
    String postStr = tspapiKey;
    postStr += "&" + str_f1 + "=";
    postStr += String(t);
    postStr += "&" + str_f2 + "=";
    postStr += String(h);
    postStr += "&" + str_f3 + "=";
    postStr += String(p);
    postStr += "\r\n\r\n";

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + tspapiKey + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(postStr.length());
    client.print("\n\n");
    client.print(postStr);
    Serial.print("\nData sent to thingspeak ");
    Serial.print(" field:");
    Serial.print(str_f1);
    Serial.print(" t:");
    Serial.print(t);
    Serial.print(" h:");
    Serial.print(h);
    Serial.print(" p:");
    Serial.print(p);

    last_tsp_milis = currentmilis;
  }
  client.stop();
}

// Return a string with easy to read time
// Convert millis to day, hr, mm, ss, ms
String easyreadtime(long millis)
{
  long dd, hh, mm, ss, ms;
  long d = 86400000;
  long h = 3600000;
  long m = 60000;
  long s = 1000;
  String retval = "";

  dd = millis / d;
  if ( dd != 0 ) retval += String(dd) + "d ";

  hh = ( millis % d) / h;
  if ( hh != 0 ) retval += String(hh) + "h ";

  mm = (( millis % d ) % h) / m;
  if ( mm != 0 ) retval += String(mm) + "m ";

  ss = ((( millis % d ) % h) % m) / s;
  retval += String(ss) + "s";

  // Serial.println(retval);
  return retval;
}

// Ez a függvény generálja
// a "root" vagy főoldalt
void handleRoot() {
  float t;
  String message = "<html><head> <title> Webhomero </title> </head>";
  String myip = "";

  currentmilis = millis();
  digitalWrite(led, 0);

  myip = WiFi.localIP().toString();
  message += "\n<body> Hello from esp8266!";
  message += "\n<br> This is sensor: ";
  message += sensorid;

  message += "\n<br><br><a href=\"http://" + myip + "/setup\"> Be&aacute;llit&aacute;sok /setup</a>";
  message += "\n<br><a href=\"http://" + myip + "/t\"> M&eacute;rt adatok /t</a>";
  message += "\n<br><a href=\"http://" + myip + "/print24\"> M&eacute;rt adatok 24 ora alatt/t</a>";
  message += "\n<br><a href=\"http://" + myip + "/collectnew\"> Uj adatok gyujtese /collectnew</a>";
  message += "\n<br><a href=\"http://" + myip + "/backlight\"> hattervilagitas be /backlight</a>";

  Serial.print("\n\nhandleRoot Uptime (s): ");
  Serial.print( easyreadtime( currentmilis ) );
  t = readadjustedtemp();
  Serial.print("\nhandleRoot Temp: ");
  Serial.print( t );
  Serial.print("\nhandleRoot Temp adjusted with: ");
  Serial.print( tempadjust);
  Serial.print("\nhandleRoot Temp firstsample: ");
  Serial.print(tempfirstsample);
  Serial.print("\nhandleRoot Humidity: ");
  Serial.print(readadjustedhum());
  Serial.print("\nhandleRoot Pressure: ");
  Serial.print(readadjustedpress());

  message += "\n<br><br> Temp: ";
  message += readadjustedtemp();
  message += "\n<br> Temp adjusted: ";
  message += tempadjust;
  message += "\n<br> Temp firstsample: ";
  message += tempfirstsample;
  message += "\n<br> Humidity: ";
  message += readadjustedhum();
  message += "\n<br> Pressure: ";
  message += readadjustedpress();

  // Wget sorok
  message += "<br><br>\n DATA;";
  message += readadjustedtemp();
  message += ";";
  message += readadjustedhum();
  message += ";";
  message += readadjustedpress();

  message += "<br><br>\n ";
  message += wifi_event;

message += "<br> Wifi SSID:";
message += WiFi.SSID();
message += "<br> Wifi signal strength (dBm): ";
message += WiFi.RSSI();

  message += "\n<br><br> Uptime : ";
  message += easyreadtime( currentmilis );
  //message += currentmilis / 1000;
  message += "\n<br> Previous rootload ";
  message += easyreadtime( currentmilis - last_rootload_milis );
  message += " ago.";

  // message += (currentmilis - last_rootload_milis) / 1000;
  message += "\n<br> Previous TSP update ";
  message += easyreadtime ( currentmilis - last_tsp_milis);
  message += " ago.";

  message += "\n<br> Last 24 hrs measures at position: ";
  message += last24curpos;

  //message += (currentmilis - last_tsp_milis) / 1000;

  message += "\n<br>\n<br> Sketch: ";
  message += buildinfo;

  message += "\n</body></html>\n";

  server.send(200, "text/html", message);
  last_rootload_milis = currentmilis;
}

void handleNotFound() {
  digitalWrite(led, 0);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// Ezt a függvényt hívjuk meg, amikor
// csak a mérési érékeket akarjuk kiírni
void handlet() {
  digitalWrite(led, 0);
  String message = "";
  message += "ID:";
  message += sensorid;
  message += ";T:";
  message += readadjustedtemp();
  message += ";H:";
  message += readadjustedhum();
  message += ";P:";
  message += readadjustedpress();
  message += ";";
  message += minmax_last24h();
  message += ";SSID:";
  message += WiFi.SSID();
  message += ";RSSI:";
  message += WiFi.RSSI();
  message += ";";
  
  server.send(200, "text/plain", message);
}

void handleprint24() {
  digitalWrite(led, 0);
  String message = print_last24h();
  server.send(200, "text/plain", message);
}

void handlecollectnewdata() {
  digitalWrite(led, 0);
  request_remote_sensor_data();
  server.send(200, "text/plain", "Collecting new info for lcd");
}


// Ezt a függvényt hívjuk meg, amikor
// be akarjuk állítani az eeprom értékeit
void handleset() {
  int i = 0;
  int commit = 0;
  String message = "Settings \n\n";
  String msg2 = "";
  //String str1;
  String s2;

  String myip = "";

  digitalWrite(led, 0);
  myip = WiFi.localIP().toString();

  digitalWrite(led, 0);

  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";

    if ( server.argName(i) == "adjust" )
    {

      s2 = (String)server.arg(i);
      tempadjust = s2.toFloat();
      myeprom.tempadjust = tempadjust;
      msg2 += "\nadjust updated: ";
      msg2 += tempadjust;
    }

    if ( server.argName(i) == "tspapikey" )
    {
      tspapiKey = (String)server.arg(i);
      myeprom.tspapiKey = tspapiKey;
      msg2 += "\ntspapiKey updated: ";
      msg2 += tspapiKey;

    }

    if ( server.argName(i) == "tspfield" )
    {

      s2 = (String)server.arg(i);
      tspfield = s2.toInt();
      myeprom.tspfield = tspfield;
      msg2 += "\ntspfield updated: ";
      msg2 += tspfield;
    }

    if ( server.argName(i) == "villogas" )
    {
      s2 = (String)server.arg(i);
      villogas = s2.toInt();
      myeprom.villogas = villogas;
      msg2 += "\nvillogas updated: ";
      msg2 += villogas;
    }

    if ( server.argName(i) == "sensorid" )
    {
      s2 = (String)server.arg(i);
      sensorid = s2.toInt();
      myeprom.sensorid = sensorid;
      msg2 += "\nvillogas updated: ";
      msg2 += sensorid;
    }


    if ( server.argName(i) == "store" )
    {
      s2 = (String)server.arg(i);
      if ( s2 == "Save and commit" )
      {
        commit = 1;
        msg2 += "\nwill commit to eeprom. ";
      }
      else
      {
        msg2 += "\nwill NOT commit to eeprom. ";
      }
    }

  }

  message += msg2;

  if ( commit == 1) eeprom_write();

  server.send(200, "text/plain", message);
}

// Altalanos beállító oldal
// Kiir egy html formot
// ahol a legfontosabb érékeket be tudjuk állítani
void handleSetup()
{
  digitalWrite(led, 0);
  String message = "<html> \
\n<head> \
\n<title> Webhomero setup </title> \
\n</head> \
\n<body> \
\n<table> \
\n<form action=\"/set\" method=\"get\"> \
\n  <tr><td>Temperature adjust: </td>\
\n  <td><input type=\"text\" name=\"adjust\" maxlength=\"5\" value=\"";
  message += tempadjust;
  message += "\"></td></tr> \
\n  <tr><td>ThingSpeak Channel: \
\n  <td><input type=\"text\" name=\"tspapikey\" value=\"";
  message += tspapiKey;
  message += "\"></td></tr> \
\n  <tr><td>ThingSpeak starting filed number (if empty skip upload) : \
\n  <td><input type=\"text\" name=\"tspfield\" maxlength=\"5\" value=\"";
  message += tspfield;
  message += "\"></td></tr> \
\n  <tr><td>Villogas: \
\n  <td><input type=\"text\" name=\"villogas\" maxlength=\"1\" value=\"";
  message += villogas;
  message += "\"></td></tr> \
\n  <tr><td>SensorID: \
\n  <td><input type=\"text\" name=\"sensorid\" maxlength=\"2\" value=\"";
  message += sensorid;
  message += "\"></td></tr> \
\n  <tr><td>\
\n  <input type=\"submit\" name=\"submit\" value=\"Save\"></td></tr> \
\n  <tr><td>\
\n  <input type=\"submit\" name=\"store\" value=\"Save and commit\"></td></tr> \
\n</form> \
\n</table> \
\n</body> \
\n<html>";

  server.send(200, "text/html", message);
}

void handlebacklight()
{
  last_backlight_on_millis = millis();
  lcd.backlight();
  handleRoot();
}


void onStationDisconnected(const WiFiEventSoftAPModeStationDisconnected& evt) {
  // nem tudom jó lesz-e
  // ha lejár a MAC lease, akkor kidob-e...

  int res = 1;
  Serial.print("Station disconnected: ");
  wifi_event += "\nWifi disconnect at (s):";
  wifi_event += millis() / 1000;
  do
  {
    res = connectToWIFI(ssid1, password1);
    if ( res != 0 ) res = connectToWIFI ( ssid2, password2);
  } while ( res != 0);


}

int connectToWIFI(const char *ssid, const char *password)
{
  int tries = 0;
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  Serial.println("\n");
  Serial.println("\nConnceting to Wifi: ");
  Serial.println(ssid);

  // Wait for connection approx 2 minutes
  while (WiFi.status() != WL_CONNECTED && tries < 120) {
    delay(500);
    tries = tries + 1;
    Serial.print(".");
  }
  if ( WiFi.status() == WL_CONNECTED)
  {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifi_event += " Wifi connected ";;
    wifi_event += millis() / 1000;
    wifi_event += " (s) after power-on";
    wifi_event += " Wifi IP:";
    wifi_event += String(WiFi.localIP().toString());
    return 0;
  }
  else
  {
    Serial.println("");
    Serial.print("Failed to connect to ");
    Serial.println(ssid);
    return 1;
  }

}

void blink()
{
  last_blink_milis = millis();
  // Az elso 50 masodpercben mindenkeppen villogunk, csak hogy tudjuk, hogy jok vagyunk
  if ( ledstatus == 1 && ( villogas == 1 || last_blink_milis < 50000 ) )  {
    digitalWrite(led, 0);
    ledstatus = 0;
  }
  else {
    digitalWrite(led, 1);
    ledstatus = 1;
  }
}

// Inicializálja azt a tombot amibe az utolso
// 24 ora legkisebb és legnagyobb hőmérsékletét
// gyűjtjük
void init_last24h()
{
  int i = 0;
  int curmillis;
  curmillis = millis();
  for (i = 0; i < 24; i++)
  {
    templast24[i].tmax = -150;
    templast24[i].tmin = 150;
    templast24[i].millis = curmillis + i * 3600000;
    templast24[i].tmaxtime = templast24[i].millis;
    templast24[i].tmintime = templast24[i].millis;
  }
  last24curpos = 0;
}

// update hourly tmin and tmax
// writes values to templast24 array
// rotates write postion if needed.
void update_last24h()
{
  long curmillis;
  long curhour; // Ennyi óra telt el a bekapcsolás óta
  int  pos;
  float t;

  curmillis = millis();
  curhour = curmillis / 3600000; // Ennyi óraja vagyunk bekapcsolva
  pos = (int) (curhour % 24); // ora mod 24 ide írjuk az értékeinket

  Serial.println("\nupdate_last24: Updating hourly tmax and tmin");
  t = readadjustedtemp();

  if ( last24curpos != pos )
  {
    Serial.println("\nupdate_last24: moving to next position with pointer");
    last24curpos = pos;
    templast24[last24curpos].millis = curmillis;
    templast24[last24curpos].tmax = t;
    templast24[last24curpos].tmin = t;
    templast24[last24curpos].tmaxtime = curmillis;
    templast24[last24curpos].tmintime = curmillis;

  }
  if ( t > templast24[last24curpos].tmax )
  {
    templast24[last24curpos].tmax = t;
    templast24[last24curpos].tmaxtime = curmillis;
    //Serial.println("\nupdate_last24: update tmax");

  }
  if ( t < templast24[last24curpos].tmin )
  {
    templast24[last24curpos].tmin = t;
    templast24[last24curpos].tmintime = curmillis;
    //Serial.println("\nupdate_last24: update tmin");
  }
}

// Segédfüggvény, kikeresi a legkisebb és
// legnagyobb hőmérsékletet
String print_last24h()
{
  int i = 0;
  int curmillis;
  String message;

  curmillis = millis();
  for (i = 0; i < 24; i++)
  {
    message += "\n";
    message += i;
    message += " tmaxtime;";
    message += easyreadtime(templast24[i].tmaxtime);
    message += ";tmax;";
    message += templast24[i].tmax;
    message += ";tmintime;";
    message += easyreadtime(templast24[i].tmintime);
    message += ";tmin;";
    message += templast24[i].tmin;
  }

  message += "\n MIN and MAX:\n";
  message += minmax_last24h();
  Serial.println(message);
  return (message);
}

String minmax_last24h()
{
  int i = 0;
  int curmillis;
  String message;
  float tmin, tmax;

  tmin = templast24[0].tmin;
  tmax = templast24[0].tmax;

  for (i = 1; i < 24; i++)
  {
    if ( tmin > templast24[i].tmin ) tmin = templast24[i].tmin;
    if ( tmax < templast24[i].tmax ) tmax = templast24[i].tmax;
  }

  message += "tmin:";
  message += tmin;
  message += ";tmax:";
  message += tmax;
  //Serial.println(message);
  return (message);
}


void lcd_display_info()
{
  int i;
  long lastreadmillisago;
  //Serial.print("\nlcd update");
  last_lcdupdate_milis=millis();
  lcd.home();
  // ez villogtatja a kepernyot, inkabb csak kiirunk ures stringet, es megnezzuk, hogy mit csinal
  //lcd.clear();
 
  // Meg egy kicsit ugy atirtuk, hogy a felso sorban mindig mutassa a homersekletet local/remote, utana sor datum/ido (NTP), utana ket sor valtozzon csak
  lcd.setCursor(0, 0);
  lcd.print("                    ");
  lcd.setCursor(0, 0);
    
  lcd.print("Ti:");
  lcd.print(sensor[0].t);
  lcd.print(" To:");
  lcd.print(sensor[1].t);

  
  for (i = 0; i < MAXSENSOR; i++)
  {
    lcd.setCursor(0, i+2);
    lcd.print("                    ");
    lcd.setCursor(0, i+2);
    lcd.print("T:");
    lcd.print(sensor[i].t);

    switch ( display_field )
    {
      case 0: 
              lcd.print(" Tmin:");
              lcd.print(sensor[i].tmin);
              break;
      case 1: 
              lcd.print(" Tmax:");
              lcd.print(sensor[i].tmax);
              break;
      case 2: 
              lcd.print(" h:");
              lcd.print(sensor[i].h);
              break;
      case 3: 
              lcd.print(" p:");
              lcd.print(sensor[i].p);
              break;
      case 4: 
              lcd.setCursor(0, i+2);
              lcd.print("                    ");
              lcd.setCursor(0, i+2);
              lastreadmillisago=millis()-sensor[i].lastconnectedmillis;
              lcd.print(sensor[i].hostname);
              lcd.print(" ");
              lcd.print(easyreadtime(lastreadmillisago));
              break;
        case 5: 
              lcd.setCursor(0, i+2);
              lcd.print("                    ");
              lcd.setCursor(0, i+2);
              lcd.print("dBm:");
              lcd.print(sensor[i].rssi);
              lcd.print(" @ ");
              lcd.print(sensor[i].ssid);
              
              break;
    }  
  }
  display_field++;
  display_field = display_field % 6;
//  lcd.setCursor(0,3);
//  lcd.print("Time2refresh: ");
//  lcd.print(easyreadtime(REMOTE_UPDATE- (currentmilis - last_tsp_milis)));

// This only refreshes time every 5 secs, not so nice
//    timeClient.update();
//    lcd.print("NTPtime: ");
//    lcd.print(timeClient.getFormattedTime());
}


void lcd_display_time(void) {
  last_lcd_time_update_millis = millis();
  lcd.home();
  lcd.setCursor(0,1);

  timeClient.update();
  
  now = timeClient.getEpochTime();
  now2 = localtime(&now);

  // a datumot csak az epoch time-bol tudjuk kiirni, mert nincs r fuggveny az NTPClient library-ban
  lcd.print(now2->tm_year+1900);
  lcd.print("/");
  if (now2->tm_mon+1 < 10) 
    lcd.print(0);
  lcd.print(now2->tm_mon+1);
  lcd.print("/");
  if (now2->tm_mday < 10) 
    lcd.print(0);
  lcd.print(now2->tm_mday);
  lcd.print(" ");  
  lcd.print(timeClient.getFormattedTime());
  
}


void setup(void) {
  int res = 1;
  currentmilis = 0;
  prevmilis = 0;
  pinMode(led, OUTPUT);
  digitalWrite(led, 0);

  //gomb pinje
  pinMode(BACKLIGHT_PIN,INPUT_PULLUP);
  
  Serial.begin(115200);

  buildinfo = display_Running_Sketch();

  Serial.print("\nInit lcd ");  // Used to type in characters

  lcd.begin(20, 4, LCD_5x8DOTS);       // initialize the lcd for 20 chars 4 lines, turn on backlight
  lcd.backlight();
  lcd.clear();
  lcd.home();
  lcd.setCursor(0, 0); //Start at character 4 on line 0
  lcd.print("Hello, world!");
  lcd.setCursor(0,1);
  lcd.print("Connecting Wifi...");

  do
  {
    res = connectToWIFI(ssid1, password1);
    if ( res != 0 ) res = connectToWIFI ( ssid2, password2);
  } while ( res != 0);

  lcd.setCursor(0,2);
  lcd.print(WiFi.localIP());

  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  // EEprom init és egy kiolvasás
  EEPROM.begin(512);

  if ( eeprom_read() == false )
  {
    Serial.print("\nEEPROM not initialized");
  }
  else
  {
    Serial.print("\nEEPROM is initialized");
    Serial.print("\nLoading data from EEPROM");
    tempadjust = myeprom.tempadjust;
    villogas = myeprom.villogas;
    tspfield = myeprom.tspfield;
    sensorid = myeprom.sensorid;
  }

  server.on("/", handleRoot);
  server.on("/set", handleset);
  server.on("/setup", handleSetup);
  server.on("/t", handlet);
  server.on("/print24", handleprint24);
  server.on("/collectnew", handlecollectnewdata);
  server.on("/backlight",handlebacklight);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("\nHTTP server started");

  Wire.begin();
  Wire.setClock(400000); //Increase to fast I2C speed!

  // inicializáljuk az utolsó 24óra mérését tartalamzó
  // tömböt;
  init_last24h();

  scan_I2Cbus();
  lcd.setCursor(0,3);
  if (fake_bme) {
    setup_fake_bme280();
    lcd.print("Si7021 sensor OK");
  } else {
    setup_bme280(i2caddr_bme280);
    lcd.print("BME280 sensor OK");
  }

  delay(2000);

  //lcd.home();
  //lcd.clear();
  //lcd.setCursor(0, 0);

  sensor[0].hostname = "localhost";
  sensor[0].status = SENSOR_IDLE;
  sensor[1].hostname = "10.1.1.31";
  sensor[1].status = SENSOR_IDLE;
 // sensor[2].hostname = "192.168.1.192";
 // sensor[2].status = SENSOR_IDLE;

  // Csak akkor csinálunk mérést, ha sikeres volt a bme280 inicializálása
  if ( init_bme280 == 0 )
  {
    logtsp();
    update_last24h();
    update_local_sensor_data();
    request_remote_sensor_data();
  }

  lcd.clear();
  // NTP Client
  timeClient.begin();

}

void request_remote_sensor_data()
{
  int i;
  char chr_hostname[100];

  for (i = 0; i < MAXSENSOR; i++)
  {
    if ( sensor[i].hostname != "localhost" )
    {
      if ( sensor[i].status == SENSOR_IDLE )
      {
        sensor[i].status = SENSOR_CONNECTING;
        sensor[i].hostname.toCharArray(chr_hostname, sensor[i].hostname.length() + 1);
        Serial.print("\nConnecting to remote sensor:");
        Serial.print(chr_hostname);
        Serial.print(":");
        if (sensor[i].client.connect(chr_hostname, 80)) {

          // Make a HTTP request:
          sensor[i].client.println("GET /t HTTP/1.1");
          sensor[i].client.println("Host: webhomero.display.local");
          sensor[i].client.println("Connection: close");
          sensor[i].client.println();
          sensor[i].lastconnectedmillis=millis();
          sensor[i].status = SENSOR_CONNECTED;
          Serial.println("\nHTTP request sent");
        } else {
          // if you didn't get a connection to the server:
          Serial.println("connection failed");
          sensor[i].client.stop();
          sensor[i].status = SENSOR_IDLE;
        }
      }
    }
  }
}

int remote_sensor_data_is_available()
{
  int i;
  for (i = 0; i < MAXSENSOR; i++)
  {
    if ( sensor[i].hostname != "localhost" )
    {
      if ( sensor[i].client.available() )
      {
        sensor[i].status = SENSOR_HASDATA;
        Serial.print("\nSensor has data:");
        Serial.print(i);
        return i;
      }
    }
    //else Serial.println("\nSkipping localhost");
  }
  return -1;
}

void read_remote_sensor_data(int i)
{
  int loc1;
  Serial.println("\nTrying to read sensor:");
  Serial.println(i);
  if ( sensor[i].status == SENSOR_HASDATA )
  {
    while (sensor[i].client.available()  ) {
      String line = sensor[i].client.readString();
      Serial.print(line);
      loc1 = line.indexOf("ID:");
      if ( loc1 != -1  )
      {
        sensor[i].datastring = String(line.substring(loc1));
        Serial.print("\nData:"); Serial.print(sensor[i].datastring);
      }
      else
      {
        Serial.println("Data not found...");
      }
      sensor[i].status = SENSOR_DATAREAD;
      Serial.print("\nSensor connection closing:");
      Serial.print(i);
      sensor[i].client.stop();
      sensor[i].status = SENSOR_IDLE;
    }

  }
}


void close_remote_sensor_connections()
{
  int i;
  for (i = 0; i < MAXSENSOR; i++)
  {
    if ( !sensor[i].client.connected() )
    {
      sensor[i].status = SENSOR_IDLE;
      Serial.println("\nSensor connection closing:");
      Serial.println(i);
      sensor[i].client.stop();
    }
  }
}

void update_local_sensor_data()
{
  float tmin, tmax;
  int i;

  //Serial.print("\nUpdating local sensor");
  tmin = templast24[0].tmin;
  tmax = templast24[0].tmax;

  for (i = 1; i < 24; i++)
  {
    if ( tmin > templast24[i].tmin ) tmin = templast24[i].tmin;
    if ( tmax < templast24[i].tmax ) tmax = templast24[i].tmax;
  }
  
  sensor[0].t=readadjustedtemp();
  sensor[0].h=readadjustedhum();
  sensor[0].p=readadjustedpress();
  sensor[0].tmin=tmin;
  sensor[0].tmax=tmax;
  sensor[0].lastconnectedmillis=millis();
  sensor[0].ssid=WiFi.SSID();
  sensor[0].rssi=WiFi.RSSI();

}
 

void update_remote_sensor_data(int sensorid)
{
  int loc1 = 0;
  int loc2 = 0;
  int i;
  String tmp;
  String inputstr;


  Serial.print("\n"); Serial.print(sensorid); Serial.print(" interpreting datastring");
  inputstr = sensor[sensorid].datastring;
  loc1 = inputstr.indexOf("ID:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 3, loc2);
    Serial.print("\nID:"); Serial.print(tmp);
    sensor[sensorid].id = tmp.toInt();
  } else Serial.println("ID: not found!");

  loc1 = inputstr.indexOf("T:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 2, loc2);
    Serial.print(" T:"); Serial.print(tmp); Serial.print(":");
    sensor[sensorid].t = tmp.toFloat();
    Serial.print(" converted to "); Serial.print(sensor[sensorid].t);

  } else  Serial.println("T: not found!");

  loc1 = inputstr.indexOf("H:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 2);
    tmp = inputstr.substring(loc1 + 2, loc2);
    Serial.print(" H:"); Serial.print(tmp);
    sensor[sensorid].h = tmp.toFloat();

  } else Serial.println("H: not found!");

  loc1 = inputstr.indexOf("P:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 2);
    tmp = inputstr.substring(loc1 + 2, loc2);
    Serial.print(" P:"); Serial.print(tmp);
    sensor[sensorid].p = tmp.toFloat();

  } else Serial.println("p: not found!");

  loc1 = inputstr.indexOf("tmin:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 5, loc2);
    Serial.print(" tmin:"); Serial.print(tmp);
    sensor[sensorid].tmin = tmp.toFloat();

  } else  Serial.println("tmin: not found!");

  loc1 = inputstr.indexOf("tmax:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 5, loc2);
    Serial.print(" tmax:"); Serial.print(tmp);
    sensor[sensorid].tmax = tmp.toFloat();

  } else Serial.println("tmax: not found!");

  loc1 = inputstr.indexOf("SSID:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 5, loc2);
    Serial.print(" SSID:"); Serial.print(tmp);
    sensor[sensorid].ssid = tmp;

  } else Serial.println("SSID: not found!");


 loc1 = inputstr.indexOf("RSSI:");
  if ( loc1 != -1 )
  {
    // Hol van a kovetkezo ;
    loc2 = inputstr.indexOf(";", loc1 + 1);
    tmp = inputstr.substring(loc1 + 5, loc2);
    Serial.print(" RSSI dBm:"); Serial.print(tmp);
    sensor[sensorid].rssi = tmp.toInt();

  } else Serial.println("RSSI: not found!");

}

void loop(void) {
  int sensorid = 1;
  int val = LOW;
  
  currentmilis = millis();
  
  server.handleClient();
  
  // Csak akkor csinálunk mérést, ha sikeres volt a bme280 inicializálása és csatlakoztunk is
  if ( init_bme280 == 0 && WiFi.status() == WL_CONNECTED )
  {
    // Log to ThinkSpeak every 300.000ms = 5min
    if ( currentmilis - last_tsp_milis > REMOTE_UPDATE )
    {
      logtsp();
      update_last24h();
      update_local_sensor_data();
      request_remote_sensor_data();
    }

    //
    // Van adat a tavoli gépeknél
    sensorid = remote_sensor_data_is_available();
    if ( sensorid != -1 )
    {
      read_remote_sensor_data(sensorid);
      update_remote_sensor_data(sensorid);
      sensorid = -1;
    }
    // Blink LED every 2 sec
    if ( currentmilis - last_blink_milis > 2000 )
    {
      blink();
      if ( debug == 1 ) readadjustedtemp();
    }

    // 5 másodpercenként van helyi mérés
    //
    if( currentmilis - last_lcdupdate_milis > 5000)
     {
      update_local_sensor_data();
      lcd_display_info();
     }

    // every 1 sec time on LCD is updated 
    if( currentmilis - last_lcd_time_update_millis > 1000)
     {
      lcd_display_time();
     }

    // 2 perc utan kikapcsoljuk a villanyt
    if( currentmilis - last_backlight_on_millis > 120000)
     {
      lcd.noBacklight();
     }

    // Kapcsoljuk be gombnyomásra az LCD vilagitast
    // ha 5 masodpercen belul mar felkapcsoltuk akkor hagyjuk
    // val = LOW ha meg van nyomva
    val = digitalRead(BACKLIGHT_PIN);
    if( val == LOW )
    {
      if( currentmilis - last_backlight_on_millis > 5000 )
      {
        lcd.backlight();
        last_backlight_on_millis = currentmilis;
      }
    }

//    timeClient.update();
//    Serial.println(timeClient.getFormattedTime());

  }
  else
  {
    // Error: Blink Led quickly every 0.5 sec
    villogas = 1;
    if ( currentmilis - last_blink_milis > 500)
    {
      blink();
    }

  }

}
