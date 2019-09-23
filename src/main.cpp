#include <Arduino.h>
#include <qrcode.h>
#include <TFT_eSPI.h>
#include <IotWebConf.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

/*
D1 -- Config

D3 -- LCD DC
D4 -- LCD RES
D5 -- LCD SCL
D7 -- LCD SDA
*/

#define CONFIG_PIN D1

QRCode qrcode;
TFT_eSPI tft = TFT_eSPI();

const char thingName[] = "BARTTimes";
const char wifiInitialApPassword[] = "password1";

DNSServer dnsServer;
WebServer server(80);

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword);

#define MIN_TIME_PARAM_LEN 5
char minTimeParamValue[MIN_TIME_PARAM_LEN];
IotWebConfParameter minTimeParam = IotWebConfParameter("Minimum time (minutes)", "minTime", minTimeParamValue, MIN_TIME_PARAM_LEN, "number", "1..100", "3", "min='0' max='100' step='1'");

void handleRoot();
void updateBartTimes();
bool goodDirection(String s);

byte iwcState = 255;
uint32_t lastUpdate = 0;

void setup() {
  Serial.begin(115200);

  tft.begin();
  tft.invertDisplay(1);

  tft.setRotation(0);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(TL_DATUM);

  iotWebConf.setConfigPin(CONFIG_PIN);

  iotWebConf.addParameter(&minTimeParam);

  iotWebConf.init();

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });


  tft.fillScreen(TFT_BLACK);
  tft.drawString("Ready.", 0, 0, 1);
  Serial.println("Ready.");
}


void loop() {
  iotWebConf.doLoop();
  byte newState = iotWebConf.getState();
  if (newState != iwcState) {
    iwcState = newState;
    if (newState == IOTWEBCONF_STATE_AP_MODE) {
      tft.fillScreen(TFT_WHITE);
      tft.setTextColor(TFT_BLACK, TFT_WHITE);
      tft.drawString("Scan to set up", 21, 4, 1);
      String s = "WIFI:S:" + String(iotWebConf.getThingNameParameter()->valueBuffer) + ";T:WPA;P:" + String(iotWebConf.getApPasswordParameter()->valueBuffer) + ";;";
      uint8_t qrcodeData[qrcode_getBufferSize(4)];
      qrcode_initText(&qrcode, qrcodeData, 4, ECC_MEDIUM, s.c_str());
      for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                tft.fillRect(21 + x*6, 28 + y*6, 6, 6, TFT_BLACK);
            }
        }
      }
    } else if (newState == IOTWEBCONF_STATE_CONNECTING) {
      tft.fillScreen(TFT_BLACK);
      String s = "Connecting to " + String(iotWebConf.getWifiSsidParameter()->valueBuffer) + "...";
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(s, 0, 0, 1);
    } else if (newState == IOTWEBCONF_STATE_ONLINE) {
      tft.fillScreen(TFT_BLACK);
      String s = "Connected!";
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(s, 0, 0, 1);
      lastUpdate = 0;
    }
  }

  uint32_t time = millis();
  switch (newState) {
    case IOTWEBCONF_STATE_ONLINE:
      if (time - lastUpdate > 30000) {
        updateBartTimes();
        lastUpdate = time;
      }
      break;
  }
}

void handleRoot() {
  if (iotWebConf.handleCaptivePortal()) {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>Configure</title></head><body>Hello world!<br>";
  s += "Go to <a href='config'>configure page</a> to change settings.";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void updateBartTimes() {
  int minTime = atoi(minTimeParamValue);

  tft.fillRect(0, 20, 240, 240, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.setCursor(0, 40, 2);


  HTTPClient http;
  http.begin("http://api.bart.gov/api/etd.aspx?cmd=etd&orig=16TH&key=MW9S-E7SL-26DU-VV8V&dir=n&json=y");
  int httpCode = http.GET();
  Serial.printf("Got response: %d\n", httpCode);
  if (httpCode == 200) {
    const size_t capacity = 4000;
    DynamicJsonDocument doc(capacity);
    deserializeJson(doc, http.getString());

    String date = doc["root"]["date"];
    String time = doc["root"]["time"];
    Serial.println(date);
    Serial.println(time);
    tft.println(date);
    tft.println(time);

    JsonObject station = doc["root"]["station"][0];
    JsonArray etd = station["etd"];
    
    String bestDestination = "";
    int bestTime = 999;
    for (auto v : etd) {
      String abbreviation = v["abbreviation"];
      if (goodDirection(abbreviation)) {
        Serial.println(abbreviation);
        tft.println(abbreviation);
        for (auto v2 : v["estimate"].as<JsonArray>()) {
          String minutesString = v2["minutes"];
          Serial.println("  " + minutesString);
          tft.println("  " + minutesString);
          int mins = 999;
          if (minutesString.equals("Leaving")) {
            mins = -1;
          } else {
            mins = atoi(minutesString.c_str());
          }
          if (mins < minTime) {
            continue;
          }
          if (bestDestination.equals("") || mins < bestTime) {
            bestDestination = abbreviation;
            bestTime = mins;
          }
        }
      } else {
        Serial.println("Skipping " + abbreviation);
        tft.println("Skipping " + abbreviation);
      }
    }

    if (!bestDestination.equals("")) {
      Serial.println("Next: " + bestDestination + " in " + bestTime + " minutes");
      tft.println("Next: " + bestDestination + " in " + bestTime + " minutes");
    }
  }
  http.end();
}

bool goodDirection(String s) {
  return
    s.equals("ASHB") ||
    s.equals("ANTC") ||
    s.equals("CONC") ||
    s.equals("DBRK") ||
    s.equals("DELN") ||
    s.equals("PLZA") ||
    s.equals("LAFY") ||
    s.equals("NBRK") ||
    s.equals("NCON") ||
    s.equals("ORIN") ||
    s.equals("PITT") ||
    s.equals("PCTR") ||
    s.equals("PHIL") ||
    s.equals("RICH") ||
    s.equals("ROCK") ||
    s.equals("WCRK");
}