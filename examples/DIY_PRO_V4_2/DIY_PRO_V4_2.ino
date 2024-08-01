/*
Important: This code is only for the DIY PRO PCB Version 4.2 that has a push button mounted.

This is the code for the AirGradient DIY PRO Air Quality Sensor with an ESP8266 Microcontroller with the SGP40 TVOC module from AirGradient.

It is a high quality sensor showing PM2.5, CO2, Temperature and Humidity on a small display and can send data over Wifi.

Build Instructions: https://www.airgradient.com/open-airgradient/instructions/diy-pro-v42/

Kits (including a pre-soldered version) are available: https://www.airgradient.com/open-airgradient/kits/

The codes needs the following libraries installed:
“WifiManager by tzapu, tablatronix” tested with version 2.0.11-beta
“U8g2” by oliver tested with version 2.32.15
"Sensirion I2C SGP41" by Sensation Version 0.1.0
"Sensirion Gas Index Algorithm" by Sensation Version 3.2.1
"Arduino-SHT" by Johannes Winkelmann Version 1.2.2
"Arduino-HomeKit-ESP8266" by paullj1 (from GitHub, this version contains some fixes compared to Mixiaoxiaos)

Configuration:
Set "Tools" > "CPU Frequency" to 160MHz.

Please set in the code below the configuration parameters.

If you have any questions please visit our forum at https://forum.airgradient.com/

If you are a school or university contact us for a free trial on the AirGradient platform.
https://www.airgradient.com/

CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License

*/


#include <AirGradient.h>
#include <WiFiManager.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>

#include <EEPROM.h>
#include "SHTSensor.h"

//#include "SGP30.h"
#include <SensirionI2CSgp41.h>
#include <NOxGasIndexAlgorithm.h>
#include <VOCGasIndexAlgorithm.h>


#include <U8g2lib.h>

constexpr uint32_t kShortPressDuration = 1000;
constexpr uint32_t kLongPressDuration = 4000;

enum ConfigState {
  TempC_PMug = 0,
  TempC_PMaqi = 1,
  TempF_PMug = 2,
  TempF_PMaqi = 3,
  ClearHomekit = 4
};

AirGradient ag = AirGradient();
SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm voc_algorithm;
NOxGasIndexAlgorithm nox_algorithm;
SHTSensor sht;

// time in seconds needed for NOx conditioning
uint16_t conditioning_s = 10;

// for persistent saving and loading
// 0-1407 is used by Arduino-KomeKit-ESP8266
int addr = 1408;
byte value;

// Display bottom right
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Replace above if you have display on top left
//U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, /* reset=*/ U8X8_PIN_NONE);


// CONFIGURATION START

//set to the endpoint you would like to use
String APIROOT = "http://hw.airgradient.com/";

// set to true to switch from Celcius to Fahrenheit
boolean inF = false;

// PM2.5 in US AQI (default ug/m3)
boolean inUSAQI = false;

// Display Position
boolean displayTop = true;

// set to true if you want to connect to wifi. You have 60 seconds to connect. Then it will go into an offline mode.
boolean connectWIFI = true;

const int port = 9926;
ESP8266WebServer server(port);

// CONFIGURATION END


unsigned long currentMillis = 0;

const int oledInterval = 5000;
unsigned long previousOled = 0;

const int sendToServerInterval = 10000;
unsigned long previoussendToServer = 0;

const int tvocInterval = 1000;
unsigned long previousTVOC = 0;
int TVOC = 0;
int NOX = 0;

const int co2Interval = 5000;
unsigned long previousCo2 = 0;
int Co2 = 0;

const int pmInterval = 5000;
unsigned long previousPm = 0;
int pm25 = 0;
int pm01 = 0;
int pm10 = 0;
int pm03PCount = 0;

const int tempHumInterval = 2500;
unsigned long previousTempHum = 0;
float temp = 0;
int hum = 0;

ConfigState configState = ConfigState::TempC_PMug;
int lastState = LOW;
int currentState;
unsigned long pressedTime  = 0;
unsigned long releasedTime = 0;


#define USE_HOMEKIT

#ifdef USE_HOMEKIT
#include <arduino_homekit_server.h>
extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cha_airquality;
extern "C" homekit_characteristic_t cha_pm25;
extern "C" homekit_characteristic_t cha_co2;
extern "C" homekit_characteristic_t cha_voc;
extern "C" homekit_characteristic_t cha_no2;
extern "C" homekit_characteristic_t cha_temperature;
extern "C" homekit_characteristic_t cha_humidity;
static uint32_t next_report_millis = 0;
#endif // USE_HOMEKIT


void setup() {
  Serial.begin(115200);
  Serial.println("Hello");
  u8g2.begin();
  sht.init();
  sht.setAccuracy(SHTSensor::SHT_ACCURACY_MEDIUM);
  //u8g2.setDisplayRotation(U8G2_R0);

  EEPROM.begin(2048);
  delay(500);

  // Load configuration from EEPROM and clamp to valid values -> homekit reset should never be in EEPROM
  int readState = String(EEPROM.read(addr)).toInt();
  configState = static_cast<ConfigState>(max(readState, 0));
  configState = static_cast<ConfigState>(min(readState, (int) ConfigState::TempF_PMaqi));

  delay(400);
  setConfig();
  Serial.println("buttonConfig: "+String(configState));
  pinMode(D7, INPUT_PULLUP);
  for (int timer = 5; timer > 0; timer--) {
    updateOLED2("Press Button", "for Config Menu", String(timer) + "...");
    currentState = digitalRead(D7);
    if (currentState == LOW) {
      updateOLED2("Entering", "Config Menu", "");
      delay(3000);
      lastState = HIGH;
      setConfig();
      inConf();
      break;
    }
    delay(1000);
  }

  if (connectWIFI)
  {
     connectToWifi();
  }

  #ifdef USE_HOMEKIT
  updateOLED2("Setting up", "Homekit", "");

  // clear the EEPROM if it isn't empty or it starts with anything but "HAP"
  char check[4];
  for (uint8_t i=0; i < 4; i++) {
    check[i] = EEPROM.read(i);
  }
  check[3] = '\0';
  if (strcmp(check, "HAP") != 0 && strlen(check) > 0) {
    Serial.println("Clearing Homekit EEPROM region");
    for (uint16_t addr=0; addr < 1408; addr++) {
      EEPROM.write(addr, 0);
    }
    EEPROM.commit();
  }

  arduino_homekit_setup(&config);
#endif // USE_HOMEKIT

  {
    server.on("/", HandleRoot);
    server.on("/metrics", HandleRoot);
    server.onNotFound(HandleNotFound);
    
    server.begin();
  }

  updateOLED2("Warming up!", "", "S/N: " + String(ESP.getChipId(), HEX));
  sgp41.begin(Wire);
  ag.CO2_Init();
  ag.PMS_Init();
  ag.TMP_RH_Init(0x44);
}

void loop() {
  server.handleClient();

  currentMillis = millis();
  updateTVOC();
  updateOLED();
  updateCo2();
  updatePm();
  updateTempHum();
  sendToServer();
  
#ifdef USE_HOMEKIT
  updateHomeKit();
#endif // USE_HOMEHIT

}

#ifdef USE_HOMEKIT
void updateHomeKit() {
  arduino_homekit_loop();

  const uint32_t now = millis();
  if (now > next_report_millis) {
    // report sensor values every 10 seconds
    next_report_millis = now + 10 * 1000;

    cha_pm25.value.float_value = pm25;
    homekit_characteristic_notify(&cha_pm25, cha_pm25.value);

    // attempt to come up with an air quality index
    cha_airquality.value.int_value = 0;  // Unknown
    // PM2.5 (as per US EPA AQ)
    if (pm25 > 0) {
      if (pm25 <= 12) {  // Excellent
        cha_airquality.value.int_value = 1;
      } else if (pm25 <= 35) {  // Good
        cha_airquality.value.int_value = 2;
      } else if (pm25 <= 55) {  // Fair
        cha_airquality.value.int_value = 3;
      } else if (pm25 <= 150) {  // Inferior
        cha_airquality.value.int_value = 4;
      } else if (pm25 > 150) {  // Poor
        cha_airquality.value.int_value = 5;
      }
    }
    // CO2 (as per Wisconsin Department of Health Services, Breeze Technologies)
    if (Co2 > 0) {
      if (Co2 <= 419) {  // Excellent ("average atmospheric concentration")
        cha_airquality.value.int_value = max(1, cha_airquality.value.int_value);
      } else if (Co2 <= 1000) {  // Good ("occupied spaces with good air exchange")
        cha_airquality.value.int_value = max(2, cha_airquality.value.int_value);
      } else if (Co2 <= 2000) {  // Fair ("complaints of drowsiness and poor air")
        cha_airquality.value.int_value = max(3, cha_airquality.value.int_value);
      } else if (Co2 <= 5000) {  // Inferior ("headaches, sleepiness, and stagnant, stale, stuffy air")
        cha_airquality.value.int_value = max(4, cha_airquality.value.int_value);
      } else if (Co2 > 5000) {  // Poor ("unusual air conditions where high levels of other gases could also be present")
        cha_airquality.value.int_value = max(5, cha_airquality.value.int_value);
      }
    }
    // XXX: include TVOC & NOX when available?
    homekit_characteristic_notify(&cha_airquality, cha_airquality.value);

    cha_co2.value.float_value = Co2;
    homekit_characteristic_notify(&cha_co2, cha_co2.value);
    cha_voc.value.float_value = TVOC;
    homekit_characteristic_notify(&cha_voc, cha_voc.value);
    cha_no2.value.float_value = NOX;
    homekit_characteristic_notify(&cha_no2, cha_no2.value);
    cha_temperature.value.float_value = temp;
    homekit_characteristic_notify(&cha_temperature, cha_temperature.value);
    cha_humidity.value.float_value = hum;
    homekit_characteristic_notify(&cha_humidity, cha_humidity.value);
  }
}
#endif // USE_HOMEKIT

void inConf(){
  setConfig();
  currentState = digitalRead(D7);
  
  const bool buttonDown = lastState == HIGH && currentState == LOW;
  const bool buttonHeld = lastState == LOW && currentState == LOW;
  const bool buttonReleased = lastState == LOW && currentState == HIGH;

  if (buttonDown) {
    pressedTime = millis();
  } else if (buttonReleased) {
    releasedTime = millis();
    long pressDuration = releasedTime - pressedTime;
    if (pressDuration < kShortPressDuration) {
      Serial.println("Increment");
      configState = static_cast<ConfigState>(configState + 1);
      if (configState > ConfigState::ClearHomekit) {
        configState = ConfigState::TempC_PMug;
      }
    }
  } else if (buttonHeld){
    long passedDuration = millis() - pressedTime;
    if (passedDuration > kLongPressDuration) {
      if (configState == ConfigState::ClearHomekit) {
        updateOLED2("Clearing", "HomeKit", "Pairing");
        delay(1000);
        for (uint16_t addr=0; addr < 1408; addr++) {
          EEPROM.write(addr, 0);
        }
        EEPROM.commit();
      } else {
        updateOLED2("Saved", "Release", "Button Now");
        delay(1000);
        EEPROM.write(addr, char(configState));
        EEPROM.commit();
      }
      for (int timer = 5; timer > 0; timer--) {
        updateOLED2("Rebooting", "in", String(timer) + "...");
        delay(1000);
      }
      ESP.restart();
    }
  }
  lastState = currentState;
  delay(100);
  inConf();
}

void setConfig() {
  switch (configState) {
    case ConfigState::TempC_PMug:
      updateOLED2("Temp. in C", "PM in ug/m3", "Long Press Saves");
      u8g2.setDisplayRotation(U8G2_R0);
      inF = false;
      inUSAQI = false;
      break;
    case ConfigState::TempC_PMaqi:
      updateOLED2("Temp. in C", "PM in US AQI", "Long Press Saves");
      u8g2.setDisplayRotation(U8G2_R0);
      inF = false;
      inUSAQI = true;
      break;
    case ConfigState::TempF_PMug:
      updateOLED2("Temp. in F", "PM in ug/m3", "Long Press Saves");
      u8g2.setDisplayRotation(U8G2_R0);
      inF = true;
      inUSAQI = false;
      break;
    case ConfigState::TempF_PMaqi:
      updateOLED2("Temp. in F", "PM in US AQI", "Long Press Saves");
      u8g2.setDisplayRotation(U8G2_R0);
      inF = true;
      inUSAQI = true;
      break;
    case ConfigState::ClearHomekit:
      updateOLED2("Long Press", "Clears", "HomeKit Pairing");
      break;
    default:
      updateOLED2("Something", "went", "wrong");
  }
}

void HandleRoot() {
  server.send(200, "text/plain", GenerateMetrics());

}

void HandleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/html", message);
}

String GenerateMetrics() {
  String message = "";
  String deviceId = String(ESP.getChipId(), HEX);
  String idString = "{id=\"" + String(deviceId) + "\",mac=\"" + WiFi.macAddress().c_str() + "\"}";

  {
    message += "# HELP rco2 CO2 value, in ppm as ug/m^3\n";
    message += "# TYPE rco2 gauge\n";
    message += "rco2";
    message += idString;
    message += String(Co2);
    message += "\n";
  }

  {
    message += "# HELP pm01 Particulate Matter PM1 value as ug/m^3\n";
    message += "# TYPE pm01 gauge\n";
    message += "pm01";
    message += idString;
    message += String(pm01);
    message += "\n";
  }

  {
    message += "# HELP pm02 Particulate Matter PM2.5 value as ug/m^3\n";
    message += "# TYPE pm02 gauge\n";
    message += "pm02";
    message += idString;
    message += String(pm25);
    message += "\n";
  }

  {
    message += "# HELP pm10 Particulate Matter PM10 value as ug/m^3\n";
    message += "# TYPE pm10 gauge\n";
    message += "pm10";
    message += idString;
    message += String(pm10);
    message += "\n";
  }

  {
    message += "# HELP pm03 Particulate Matter PM0.3 count\n";
    message += "# TYPE pm03 gauge\n";
    message += "pm03";
    message += idString;
    message += String(pm03PCount);
    message += "\n";
  }

  {
    message += "# HELP tvoc Total Volatile Organic Compounts, in parts per billion\n";
    message += "# TYPE tvoc gauge\n";
    message += "voc";
    message += idString;
    message += String(TVOC);
    message += "\n# HELP nox Nitric Oxide, in parts per billion\n";
    message += "# TYPE nox gauge\n";
    message += "nox";
    message += idString;
    message += String(NOX);
    message += "\n";
  }

  {
    message += "# HELP atmp Temperature, in degrees Celsius\n";
    message += "# TYPE atmp gauge\n";
    message += "atmp";
    message += idString;
    message += String(temp);
    message += "\n# HELP rhum Relative humidity, in percent\n";
    message += "# TYPE rhum gauge\n";
    message += "rhum";
    message += idString;
    message += String(hum);
    message += "\n";
  }

  return message;
}


void updateTVOC()
{
 uint16_t error;
    char errorMessage[256];
    uint16_t defaultRh = 0x8000;
    uint16_t defaultT = 0x6666;
    uint16_t srawVoc = 0;
    uint16_t srawNox = 0;
    uint16_t defaultCompenstaionRh = 0x8000;  // in ticks as defined by SGP41
    uint16_t defaultCompenstaionT = 0x6666;   // in ticks as defined by SGP41
    uint16_t compensationRh = 0;              // in ticks as defined by SGP41
    uint16_t compensationT = 0;               // in ticks as defined by SGP41

    delay(1000);

    compensationT = static_cast<uint16_t>((temp + 45) * 65535 / 175);
    compensationRh = static_cast<uint16_t>(hum * 65535 / 100);

    if (conditioning_s > 0) {
        error = sgp41.executeConditioning(compensationRh, compensationT, srawVoc);
        conditioning_s--;
    } else {
        error = sgp41.measureRawSignals(compensationRh, compensationT, srawVoc,
                                        srawNox);
    }

    if (currentMillis - previousTVOC >= tvocInterval) {
      previousTVOC += tvocInterval;
      TVOC = voc_algorithm.process(srawVoc);
      NOX = nox_algorithm.process(srawNox);
      Serial.println(String(TVOC));
    }
}

void updateCo2()
{
    if (currentMillis - previousCo2 >= co2Interval) {
      previousCo2 += co2Interval;
      Co2 = ag.getCO2_Raw();
      Serial.println(String(Co2));
    }
}

void updatePm()
{
    if (currentMillis - previousPm >= pmInterval) {
      previousPm += pmInterval;
      pm01 = ag.getPM1_Raw();
      pm25 = ag.getPM2_Raw();
      pm10 = ag.getPM10_Raw();
      pm03PCount = ag.getPM0_3Count();
      Serial.println(String(pm25));
    }
}

void updateTempHum()
{
    if (currentMillis - previousTempHum >= tempHumInterval) {
      previousTempHum += tempHumInterval;
      if (sht.readSample()) {
        temp = sht.getTemperature();
        hum = sht.getHumidity();
      } else {
          Serial.print("Error in readSample()\n");
      }
    }
}

void updateOLED() {
   if (currentMillis - previousOled >= oledInterval) {
     previousOled += oledInterval;

    String ln3;
    String ln1;

    if (inUSAQI) {
      ln1 = "AQI:" + String(PM_TO_AQI_US(pm25)) +  " CO2:" + String(Co2);
    } else {
      ln1 = "PM:" + String(pm25) +  " CO2:" + String(Co2);
    }

     String ln2 = "TVOC:" + String(TVOC) + " NOX:" + String(NOX);

      if (inF) {
        ln3 = "F:" + String((temp* 9 / 5) + 32) + " H:" + String(hum)+"%";
        } else {
        ln3 = "C:" + String(temp) + " H:" + String(hum)+"%";
       }
     updateOLED2(ln1, ln2, ln3);
   }
}

void updateOLED2(String ln1, String ln2, String ln3) {
  char buf[9];
  u8g2.firstPage();
  u8g2.firstPage();
  do {
  u8g2.setFont(u8g2_font_t0_16_tf);
  u8g2.drawStr(1, 10, String(ln1).c_str());
  u8g2.drawStr(1, 30, String(ln2).c_str());
  u8g2.drawStr(1, 50, String(ln3).c_str());
    } while ( u8g2.nextPage() );
}

void sendToServer() {
   if (currentMillis - previoussendToServer >= sendToServerInterval) {
     previoussendToServer += sendToServerInterval;
      String payload = "{\"wifi\":" + String(WiFi.RSSI())
      + (Co2 < 0 ? "" : ", \"rco2\":" + String(Co2))
      + (pm01 < 0 ? "" : ", \"pm01\":" + String(pm01))
      + (pm25 < 0 ? "" : ", \"pm02\":" + String(pm25))
      + (pm10 < 0 ? "" : ", \"pm10\":" + String(pm10))
      + (pm03PCount < 0 ? "" : ", \"pm003_count\":" + String(pm03PCount))
      + (TVOC < 0 ? "" : ", \"tvoc_index\":" + String(TVOC))
      + (NOX < 0 ? "" : ", \"nox_index\":" + String(NOX))
      + ", \"atmp\":" + String(temp)
      + (hum < 0 ? "" : ", \"rhum\":" + String(hum))
      + "}";

      if(WiFi.status()== WL_CONNECTED){
        Serial.println(payload);
        String POSTURL = APIROOT + "sensors/airgradient:" + String(ESP.getChipId(), HEX) + "/measures";
        Serial.println(POSTURL);
        WiFiClient client;
        HTTPClient http;
        http.begin(client, POSTURL);
        http.addHeader("content-type", "application/json");
        int httpCode = http.POST(payload);
        String response = http.getString();
        Serial.println(httpCode);
        Serial.println(response);
        http.end();
      }
      else {
        Serial.println("WiFi Disconnected");
      }
   }
}

// Wifi Manager
 void connectToWifi() {
   WiFiManager wifiManager;
   //WiFi.disconnect(); //to delete previous saved hotspot
   String HOTSPOT = "AG-" + String(ESP.getChipId(), HEX);
   updateOLED2("90s to connect", "to Wifi Hotspot", HOTSPOT);
   wifiManager.setTimeout(90);

   if (!wifiManager.autoConnect((const char * ) HOTSPOT.c_str())) {
     updateOLED2("booting into", "offline mode", "");
     Serial.println("failed to connect and hit timeout");
     delay(6000);
   }

}

// Calculate PM2.5 US AQI
int PM_TO_AQI_US(int pm02) {
  if (pm02 <= 12.0) return ((50 - 0) / (12.0 - .0) * (pm02 - .0) + 0);
  else if (pm02 <= 35.4) return ((100 - 50) / (35.4 - 12.0) * (pm02 - 12.0) + 50);
  else if (pm02 <= 55.4) return ((150 - 100) / (55.4 - 35.4) * (pm02 - 35.4) + 100);
  else if (pm02 <= 150.4) return ((200 - 150) / (150.4 - 55.4) * (pm02 - 55.4) + 150);
  else if (pm02 <= 250.4) return ((300 - 200) / (250.4 - 150.4) * (pm02 - 150.4) + 200);
  else if (pm02 <= 350.4) return ((400 - 300) / (350.4 - 250.4) * (pm02 - 250.4) + 300);
  else if (pm02 <= 500.4) return ((500 - 400) / (500.4 - 350.4) * (pm02 - 350.4) + 400);
  else return 500;
};
