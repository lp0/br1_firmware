/*
 * br1_firmware - for http://timhawes.com/br1 PCB (modified to use UART1 pin GPIO2)
 *
 * Arduino packages:
 *   https://github.com/esp8266/Arduino/
 *
 * Libraries:
 *   https://github.com/lp0/NeoPixelBus/tree/UartDriven (forked from https://github.com/Makuna/NeoPixelBus/tree/UartDriven)
 *
 */

#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <NeoPixelBus.h>

#ifdef ESP8266
extern "C" {
#include "user_interface.h"
}
#endif

// #define DEBUG
// #define DEBUG_PACKETS

#ifdef DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTDEC(x) Serial.print(x, DEC)
#define DEBUG_PRINTHEX(x) Serial.print(x, HEX)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTDEC(x)
#define DEBUG_PRINTHEX(x)
#define DEBUG_PRINTLN(x)
#endif

#define MAX_PIXELS 1024

#define HUE_MAX 360
#define HUE_EXP_RANGE 60
#define HUE_EXP_TIMES 2
#define HUE_EXP_MAX ((HUE_MAX - HUE_EXP_RANGE) + (HUE_EXP_TIMES * HUE_EXP_RANGE))

const uint8_t irPin = 14;
const uint8_t buttonPin = 13;
const uint8_t dataPin = 2;
const unsigned int udpPort = 2812;

const char *wifiModeNames[] = { "STA", "AP", NULL };

const char *colourOrderNames[] = { "RGB", "GRB", "BRG", NULL };
const uint16_t colourOrderValues[] = { NEO_RGB, NEO_GRB, NEO_BRG };

const char *modeNames[] = { "Black", "Red", "Dim red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White", "HSV Scroll", "HSV Fade", "Christmas (Red and Green)", "Twinkle", "Red night light", "Christmas (Work Colours)", NULL };

struct EepromData {
  uint8_t configured;
  char ssid[128];
  char passphrase[128];
  uint16_t pixelcount;
  uint16_t colourorder;
  uint8_t scalered;
  uint8_t scalegreen;
  uint8_t scaleblue;
  uint8_t defaultmode;
  uint8_t wifimode;
} eepromData;

char myhostname[64];
IPAddress ip;
NeoPixelBus pixels;
ESP8266WebServer server(80);
WiFiUDP Udp;
uint8_t inboundMessage[1500];
uint8_t ledMode = 9;
boolean ledModeChanged = false;
uint8_t buttonState = HIGH;

void configuration_mode();
void run_mode();
void udpLoop();
void tcpLoop();
void ledLoop();
void singleColour(uint8_t red, uint8_t green, uint8_t blue);

void setup() {
  boolean configMode = false;

  pinMode(buttonPin, INPUT_PULLUP);

  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting - Hold button to enter configuration mode");
  Serial.print("getResetInfo=");
  Serial.println(ESP.getResetInfo());

  snprintf(myhostname, sizeof(myhostname), "ws2812-%08x", ESP.getChipId());
  wifi_station_set_hostname(myhostname);

  EEPROM.begin(512);
  EEPROM.get(0, eepromData);

  if (eepromData.configured != 1) {
    Serial.println("EEPROM is empty, going into configuration mode");
    configMode = true;
  } else if (digitalRead(buttonPin) == LOW) {
    delay(500);
    if (digitalRead(buttonPin) == LOW) {
      Serial.println("Button pressed, going into configuration mode");
      configMode = true;
    }
  }

  if (configMode) {
    configuration_mode();
  } else {
    Serial.println("Entering run mode");
    run_mode();
  }
}

void loop() {
  static boolean waitingForWiFi = true;

  if (eepromData.wifimode != 1 && waitingForWiFi) {
    if (WiFi.status() == WL_CONNECTED) {
      ip = WiFi.localIP();
      Serial.print("WiFi ready: SSID=");
      Serial.print(eepromData.ssid);
      Serial.print(" IP=");
      Serial.print(ip);
      Serial.print(" HOSTNAME=");
      Serial.println(myhostname);
      waitingForWiFi = false;
    }
  }

  udpLoop();
  tcpLoop();

  // give time to the ESP8266 WiFi stack
  yield();

  ledLoop();

  uint8_t newButtonState = digitalRead(buttonPin);
  if ((newButtonState == LOW) && (buttonState == HIGH)) {
    ++ledMode;
    ledModeChanged = true;
    Serial.print("Button pressed, mode=");
    Serial.println(ledMode, DEC);
    buttonState = LOW;
  } else if ((newButtonState == HIGH) && (buttonState == LOW)) {
    buttonState = HIGH;
  }

  // give time to the ESP8266 WiFi stack
  yield();
}

void udpMessageHandler(int len) {
  switch (inboundMessage[0]) {
  case 0x01: {
    // static colour: r, g, b
    singleColour(inboundMessage[1], inboundMessage[2], inboundMessage[3]);
    ledMode = 255;
    break;
  }
  case 0x02:
    // preset mode: n
    ledMode = inboundMessage[1];
    ledModeChanged = true;
    break;
  case 0x03: {
    // full sequence: r, g, b, r, g, b, ...
    uint8_t red, green, blue;
    uint16_t pos = 1;
    uint16_t pixel = 0;
    while ((pos + 2) < len) {
      red = inboundMessage[pos];
      green = inboundMessage[pos + 1];
      blue = inboundMessage[pos + 2];
      pixels.SetPixelColor(pixel, RgbColor(red, green, blue));
      pixel++;
      pos += 3;
    }
    pixels.Show();
    ledMode = 255;
    break;
  }
  }
}

void udpLoop() {
  unsigned int packetSize;
  IPAddress remoteIp;
  int remotePort;
  int len = 0;

  packetSize = Udp.parsePacket();
  if (packetSize > 0) {
    remoteIp = Udp.remoteIP();
    remotePort = Udp.remotePort();

#ifdef DEBUG_PACKETS
    Serial.print("Packet received size=");
    Serial.print(packetSize);
    Serial.print(" from=");
    Serial.print(remoteIp);
    Serial.print(":");
    Serial.print(remotePort);
#endif

    len = Udp.read(inboundMessage, sizeof(inboundMessage));

#ifdef DEBUG_PACKETS
    if (len > 0) {
      Serial.print(" data=");
      for (int i = 0; i < len; i++) {
        if (inboundMessage[i] < 16) {
          Serial.print('0');
        }
        Serial.print(inboundMessage[i], HEX);
        Serial.print(' ');
      }
    }
    Serial.println();
#endif
    udpMessageHandler(len);
  }
}

void tcpLoop() {
  server.handleClient();
}

void singleColour(uint8_t red, uint8_t green, uint8_t blue) {

  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, RgbColor(red * eepromData.scalered / 255,
                                         green * eepromData.scalegreen / 255,
                                         blue * eepromData.scaleblue / 255));
  }
  pixels.Show();
}

void redNightLight() {
  static uint8_t level = 255;
  static unsigned long lastChange = 0;
  const uint8_t minimum = 63;
  const unsigned long runtime = 1800000;
  unsigned long interval = runtime / (255 - minimum);

  if (ledModeChanged) {
    level = 255;
    singleColour(level, 0, 0);
    ledModeChanged = false;
    lastChange = millis();
  } else {
    if (millis() - lastChange > interval) {
      if (level > minimum) {
        level = level - 1;
      }
      singleColour(level, 0, 0);
      lastChange = millis();
    }
  }
}

// Convert a given HSV (Hue Saturation Value) to RGB(Red Green Blue) and set
// the led to the color
//  h is hue value, integer between 0 and 360
//  s is saturation value, double between 0 and 1
//  v is value, double between 0 and 1
// http://splinter.com.au/blog/?p=29
RgbColor ledHSV(int h, double s, double v) {
  double r = 0;
  double g = 0;
  double b = 0;

  double hf = h / 60.0;

  int i = floor(hf);
  double f = hf - i;
  double pv = v * (1 - s);
  double qv = v * (1 - s * f);
  double tv = v * (1 - s * (1 - f));

  switch (i) {
  case 0:
    r = v;
    g = tv;
    b = pv;
    break;
  case 1:
    r = qv;
    g = v;
    b = pv;
    break;
  case 2:
    r = pv;
    g = v;
    b = tv;
    break;
  case 3:
    r = pv;
    g = qv;
    b = v;
    break;
  case 4:
    r = tv;
    g = pv;
    b = v;
    break;
  case 5:
    r = v;
    g = pv;
    b = qv;
    break;
  }

  // set each component to a integer value between 0 and 255
  int red = constrain((int)255 * r, 0, 255);
  int green = constrain((int)255 * g, 0, 255);
  int blue = constrain((int)255 * b, 0, 255);

  return RgbColor(red * eepromData.scalered / 255,
                      green * eepromData.scalegreen / 255,
                      blue * eepromData.scaleblue / 255);
}

RgbColor ledExpHSV(int h, double s, double v) {
  if (h < (HUE_EXP_TIMES * HUE_EXP_RANGE))
    return ledHSV(h / HUE_EXP_TIMES, s, v);
  else
    return ledHSV(h - ((HUE_EXP_TIMES - 1) * HUE_EXP_RANGE), s, v);
}

void hsvFade() {
  static int hue = 0;
  static unsigned long lastChange = 0;
  unsigned long interval = 50;

  if (millis() - lastChange > interval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      pixels.SetPixelColor(i, ledExpHSV(hue, 1.0, 1.0));
    }
    pixels.Show();
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
  }
}

void hsvStatic() {

  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, ledExpHSV(i * HUE_EXP_MAX / eepromData.pixelcount, 1.0, 1.0));
  }
  pixels.Show();
}

void hsvScroll() {
  static int hue = 0;
  static unsigned long lastChange = 0;
  unsigned long interval = 50;

  if (millis() - lastChange > interval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      pixels.SetPixelColor(i, ledExpHSV(((i * HUE_EXP_MAX / eepromData.pixelcount) + hue) % HUE_EXP_MAX, 1.0, 1.0));
    }
    pixels.Show();
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
  }
}

void christmasRedAndGreen() {
  static unsigned long lastChange = 0;
  unsigned long interval = 200;
  const RgbColor red = RgbColor(eepromData.scalered, 0, 0);
  const RgbColor green = RgbColor(0, eepromData.scalegreen, 0);

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++) {
      if (random(0, 2) == 0) {
        pixels.SetPixelColor(i, red);
      } else {
        pixels.SetPixelColor(i, green);
      }
    }
    pixels.Show();
    ledModeChanged = false;
  }

  if (millis() - lastChange > interval) {
    int i = random(0, eepromData.pixelcount);
    if (pixels.GetPixelColor(i) == green) {
      pixels.SetPixelColor(i, red);
    } else {
      pixels.SetPixelColor(i, green);
    }
    pixels.Show();
    lastChange = millis();
  }
}

void twinkle() {
  static unsigned long lastChange = 0;
  static unsigned long lastPulse = 0;
  static uint8_t levels[MAX_PIXELS];
  static uint8_t current[MAX_PIXELS];
  unsigned long pulseInterval = 250;
  unsigned long changeInterval = 10;
  const uint8_t low = 50;
  const uint8_t high = 255;
  const uint8_t step = 10;

  if (ledModeChanged) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      levels[i] = low;
      current[i] = 0;
    }
    ledModeChanged = false;
  }

  if (millis() - lastPulse > pulseInterval) {
    int target = random(0, eepromData.pixelcount);
    if (current[target] == 0 && levels[target] == low) {
      current[target] = 1;
    }
    lastPulse = millis();
  }

  if (millis() - lastChange > changeInterval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      if (current[i] == 1) {
        // this pixel is rising
        if (levels[i] + step > high) {
          // the pixel has reached maximum
          levels[i] = high;
          current[i] = 0;
        } else {
          levels[i] = levels[i] + step;
        }
      } else if (levels[i] > low) {
        // this pixel is falling
        if (levels[i] - step < low) {
          levels[i] = low;
        } else {
          levels[i] = levels[i] - step;
        }
      }
      pixels.SetPixelColor(i, RgbColor(
        levels[i] * eepromData.scalered / 255,
        levels[i] * eepromData.scalegreen / 255,
        levels[i] * eepromData.scaleblue / 255));
    }
    pixels.Show();
    lastChange = millis();
  }
}

void christmasWork() {
  static unsigned long lastChange = 0;
  unsigned long interval = 200;
  const RgbColor grey = RgbColor(74 * eepromData.scalered / 255, 79 * eepromData.scalegreen / 255, 85 * eepromData.scaleblue / 255);
  const RgbColor red = RgbColor(194 * eepromData.scalered / 255, 4 * eepromData.scalegreen / 255, 24 * eepromData.scaleblue / 255);
  const RgbColor lightBlue = RgbColor(0, 195 * eepromData.scalegreen / 255, 215 * eepromData.scaleblue / 255);
  const RgbColor darkBlue = RgbColor(0, 51 * eepromData.scalegreen / 255, 161 * eepromData.scaleblue / 255);
  const RgbColor colours[4] = { grey, red, lightBlue, darkBlue };
  const int num_colours = sizeof(colours)/sizeof(RgbColor);

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++)
      pixels.SetPixelColor(i, colours[random(0, num_colours)]);
    pixels.Show();
    ledModeChanged = false;
  }

  if (millis() - lastChange > interval) {
    int i = random(0, eepromData.pixelcount);
    int n = random(1, num_colours);
    for (int j = 0; j < num_colours; j++) {
      if (pixels.GetPixelColor(i) == colours[j]) {
        pixels.SetPixelColor(i, colours[(j + n) % num_colours]);
        break;
      }
    }
    pixels.Show();
    lastChange = millis();
  }
}

void ledLoop() {

  switch (ledMode) {
  case 0:
    // black
    singleColour(0, 0, 0);
    break;
  case 1:
    // red
    singleColour(255, 0, 0);
    break;
  case 2:
    // dim red
    singleColour(63, 0, 0);
    break;
  case 3:
    // green
    singleColour(0, 255, 0);
    break;
  case 4:
    // yellow
    singleColour(255, 255, 0);
    break;
  case 5:
    // blue
    singleColour(0, 0, 255);
    break;
  case 6:
    // magenta
    singleColour(255, 0, 255);
    break;
  case 7:
    // cyan
    singleColour(0, 255, 255);
    break;
  case 8:
    // white
    singleColour(255, 255, 255);
    break;
  case 9:
    hsvScroll();
    break;
  case 10:
    hsvFade();
    break;
  case 11:
    christmasRedAndGreen();
    break;
  case 12:
    twinkle();
    break;
  case 13:
    redNightLight();
    break;
  case 14:
    christmasWork();
    break;
  case 255:
    // network mode - no action
    break;
  default:
    // default to black
    singleColour(0, 0, 0);
    ledMode = 0;
  }
}

void runRootHandler() {
  int i;

  String form;

  form.reserve(808);
  form += "<!DOCTYPE html>"
      "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
      "<form method=\"POST\" action=\"apply\">";

  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "default") {
      form += "<input type=\"hidden\" name=\"default\" value=\"1\"/>";
    }
  }

  form += "Mode: <select name=\"ledmode\">";
  for (i = 0; modeNames[i] != NULL; i++) {
    form += "<option value=\"";
    form += i;
    form += "\"";
    if (ledMode == i)
      form += " selected";
    form += ">";
    form += modeNames[i];
    form += "</option>";
  }
  form += "</select>";

  form += "<br/>"
      "<input type=\"submit\"/></form>";

  server.send(200, "text/html", form);
}

void runUpdateHandler() {
  int setDefault = 0;

  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "default") {
      setDefault = 1;
    }

    if (server.argName(i) == "ledmode") {
      ledMode = server.arg(i).toInt();
      ledModeChanged = true;
      Serial.print("Updated mode=");
      Serial.println(ledMode, DEC);
    }
  }

  if (setDefault) {
    eepromData.defaultmode = ledMode;
    EEPROM.put(0, eepromData);
    EEPROM.commit();

    server.send(200, "text/html", "<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"5;URL=/\"></head><p>Updated default</p>");
  } else {
    server.send(200, "text/html", "<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"0;URL=/\"></head><p>Updated</p>");
  }
}

void runConfigHandler() {
  String form;

  form.reserve(480);
  form += "<!DOCTYPE html>"
      "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
      "<form method=\"POST\" action=\"apply2\">";

  form += "Scaling: "
      "R<input type=\"number\" name=\"scalered\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scalered;
  form += "\"/>"
      "G<input type=\"number\" name=\"scalegreen\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scalegreen;
  form += "\"/>"
      "B<input type=\"number\" name=\"scaleblue\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scaleblue;
  form += "\"/><br/>";

  form += "Default Mode: <input type=\"number\" name=\"defaultmode\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.defaultmode;
  form += "\"/><br/>";
  form += "<input type=\"submit\" /></form>";

  server.send(200, "text/html", form);
}

void runConfigUpdateHandler() {
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "scalered") {
      eepromData.scalered = server.arg(i).toInt();
    }
    if (server.argName(i) == "scalegreen") {
      eepromData.scalegreen = server.arg(i).toInt();
    }
    if (server.argName(i) == "scaleblue") {
      eepromData.scaleblue = server.arg(i).toInt();
    }
    if (server.argName(i) == "defaultmode") {
      eepromData.defaultmode = server.arg(i).toInt();
    }
  }
  ledModeChanged = true;
  EEPROM.put(0, eepromData);
  EEPROM.commit();

  server.send(200, "text/html", "<p>Settings updated</p>");
}

void runUptimeHandler() {
  unsigned long uptime = millis();
  unsigned long days, hours, minutes, seconds, ms;
  char response[64];

  days = uptime / 86400000;
  uptime %= 86400000;

  hours = uptime / 3600000;
  uptime %= 3600000;

  minutes = uptime / 60000;
  uptime %= 60000;

  seconds = uptime / 1000;
  uptime %= 1000;

  ms = uptime;

  snprintf(response, sizeof(response), "%03ld+%02ld:%02ld:%02ld.%03ld\n", days, hours, minutes, seconds, ms);
  server.send(200, "text/plain", response);
}

void run_mode() {
    switch (eepromData.wifimode) {
    case 1:
      // go into access point mode
      WiFi.mode(WIFI_AP);
      if (strlen(eepromData.passphrase) > 0)
        WiFi.softAP(eepromData.ssid, eepromData.passphrase);
      else
        WiFi.softAP(eepromData.ssid);
      delay(100);
      ip = WiFi.softAPIP();

      // display access details
      Serial.print("WiFi AP: SSID=");
      Serial.print(eepromData.ssid);
      Serial.print(" URL=http://");
      Serial.print(ip);
      Serial.println("/");
      break;

    case 0:
    default:
      WiFi.mode(WIFI_STA);
      WiFi.begin(eepromData.ssid, eepromData.passphrase);
      break;
    }

    pixels.setPixelCount(eepromData.pixelcount);
    pixels.setType(eepromData.colourorder | NEO_KHZ800);
    pixels.Begin();

    Udp.begin(udpPort);
    server.on("/", runRootHandler);
    server.on("/apply", runUpdateHandler);
    server.on("/config", runConfigHandler);
    server.on("/apply2", runConfigUpdateHandler);
    server.on("/uptime", runUptimeHandler);
    server.begin();
    ledMode = eepromData.defaultmode;
    Serial.print("Ready, mode=");
    Serial.println(ledMode, DEC);
}

void configRootHandler() {
  int i;

  String form = "<!DOCTYPE html>"
      "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
      "<form method=\"POST\" action=\"update\">SSID: <input type=\"text\" "
      "name=\"ssid\"";
  if (eepromData.configured == 1)
    form += " value=\"*\"";
  form += "/><br/>"
      "Passphrase: <input type=\"text\" name=\"passphrase\" ";
  if (eepromData.configured == 1)
    form += " value=\"*\"";
  form += "/><br/>";

  form += "WiFi Mode: <select name=\"wifimode\">";
  for (i = 0; wifiModeNames[i] != NULL; i++) {
    form += "<option value=\"";
    form += i;
    form += "\"";
    if (eepromData.configured == 1 && eepromData.wifimode == i)
      form += " selected";
    form += ">";
    form += wifiModeNames[i];
    form += "</option>";
  }
  form += "</select><br/>";

  form += "Pixel Count: <input type=\"number\" name=\"pixelcount\" min=\"1\"";
  if (eepromData.configured == 1) {
    form += " value=\"";
    form += eepromData.pixelcount;
    form += "\"";
  }
  form += "/><br/>";

  form += "Colour Order: <select name=\"colourorder\">";
  for (i = 0; colourOrderNames[i] != NULL; i++) {
    form += "<option value=\"";
    form += colourOrderValues[i];
    form += "\"";
    if (eepromData.configured == 1 && eepromData.colourorder == colourOrderValues[i])
      form += " selected";
    form += ">";
    form += colourOrderNames[i];
    form += "</option>";
  }
  form += "</select><br/>";

  form += "Scaling: "
      "R<input type=\"number\" name=\"scalered\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scalered : 255);
  form += "\"/>"
      "G<input type=\"number\" name=\"scalegreen\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scalegreen : 255);
  form += "\"/>"
      "B<input type=\"number\" name=\"scaleblue\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scaleblue : 255);
  form += "\"/><br/>";

  form += "Default Mode: <input type=\"number\" name=\"defaultmode\" min=\"0\" max=\"255\" value=\"";
  if (eepromData.configured == 1) {
    form += eepromData.defaultmode;
  } else {
    form += ledMode;
  }
  form += "\"/><br/>";
  form += "<input type=\"submit\" /></form>";

  server.send(200, "text/html", form);
}

void configUpdateHandler() {

  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "ssid" && server.arg(i) != "*") {
      server.arg(i).toCharArray(eepromData.ssid, sizeof(eepromData.ssid));
    }
    if (server.argName(i) == "passphrase" && server.arg(i) != "*") {
      server.arg(i).toCharArray(eepromData.passphrase, sizeof(eepromData.passphrase));
    }
    if (server.argName(i) == "pixelcount") {
      eepromData.pixelcount = server.arg(i).toInt();
    }
    if (server.argName(i) == "colourorder") {
      eepromData.colourorder = server.arg(i).toInt();
    }
    if (server.argName(i) == "scalered") {
      eepromData.scalered = server.arg(i).toInt();
    }
    if (server.argName(i) == "scalegreen") {
      eepromData.scalegreen = server.arg(i).toInt();
    }
    if (server.argName(i) == "scaleblue") {
      eepromData.scaleblue = server.arg(i).toInt();
    }
    if (server.argName(i) == "defaultmode") {
      eepromData.defaultmode = server.arg(i).toInt();
    }
    if (server.argName(i) == "wifimode") {
      eepromData.wifimode = server.arg(i).toInt();
    }
  }
  eepromData.configured = 1;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
  server.send(200, "text/html", "<p>Settings updated</p>");

  // set first three pixels to Red-Green-Blue using the updated configuration
  // set the last pixel to White
  pixels.setPixelCount(eepromData.pixelcount);
  pixels.setType(eepromData.colourorder | NEO_KHZ800);
  pixels.Begin();
  singleColour(0, 0, 0);
  pixels.SetPixelColor(0, RgbColor(eepromData.scalered, 0, 0));
  pixels.SetPixelColor(1, RgbColor(0, eepromData.scalegreen, 0));
  pixels.SetPixelColor(2, RgbColor(0, 0, eepromData.scaleblue));
  pixels.SetPixelColor(eepromData.pixelcount - 1,
    RgbColor(eepromData.scalered, eepromData.scalegreen, eepromData.scaleblue));
  pixels.Show();
}

void configuration_mode() {
  Serial.print("freeHeap=");
  Serial.println(ESP.getFreeHeap());
  Serial.print("chipId=");
  Serial.println(ESP.getChipId(), HEX);
  Serial.print("sdkVersion=");
  Serial.println(ESP.getSdkVersion());
  Serial.print("bootVersion=");
  Serial.println(ESP.getBootVersion());
  Serial.print("bootMode=");
  Serial.println(ESP.getBootMode());
  Serial.print("cpuFreqMHz=");
  Serial.println(ESP.getCpuFreqMHz());
  Serial.print("flashChipId=");
  Serial.println(ESP.getFlashChipId(), HEX);
  Serial.print("flashChipRealSize=");
  Serial.println(ESP.getFlashChipRealSize());
  Serial.print("flashChipSpeed=");
  Serial.println(ESP.getFlashChipSpeed());
  Serial.print("sketchSize=");
  Serial.println(ESP.getSketchSize());
  Serial.print("freeSketchSpace=");
  Serial.println(ESP.getFreeSketchSpace());
  Serial.println();

  Serial.print("ssid=");
  Serial.println(eepromData.ssid);
  Serial.print("passphrase=");
  Serial.println(eepromData.passphrase);
  Serial.print("pixelcount=");
  Serial.println(eepromData.pixelcount);
  Serial.print("colourorder=");
  Serial.println(eepromData.colourorder);
  Serial.print("scalered=");
  Serial.println(eepromData.scalered);
  Serial.print("scalegreen=");
  Serial.println(eepromData.scalegreen);
  Serial.print("scaleblue=");
  Serial.println(eepromData.scaleblue);
  Serial.print("defaultmode=");
  Serial.println(eepromData.defaultmode);
  Serial.print("wifimode=");
  Serial.println(eepromData.wifimode);
  Serial.println();

  // set first three pixels to dim white to acknowledge configuration mode
  pixels.setPixelCount(3);
  pixels.setType(NEO_RGB | NEO_KHZ800);
  pixels.Begin();
  pixels.SetPixelColor(0, RgbColor(63, 63, 63));
  pixels.SetPixelColor(1, RgbColor(63, 63, 63));
  pixels.SetPixelColor(2, RgbColor(63, 63, 63));
  pixels.Show();

  // go into access point mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(myhostname);
  delay(100);
  ip = WiFi.softAPIP();

  // display access details
  Serial.print("WiFi AP: SSID=");
  Serial.print(myhostname);
  Serial.print(" URL=http://");
  Serial.print(ip);
  Serial.println("/");

  // set first three pixels to Red-Green-Blue to indicate that configuration
  // mode AP is ready, and to help the user identify the correct colour order
  pixels.SetPixelColor(0, RgbColor(255, 0, 0));
  pixels.SetPixelColor(1, RgbColor(0, 255, 0));
  pixels.SetPixelColor(2, RgbColor(0, 0, 255));
  pixels.Show();

  server.on("/", configRootHandler);
  server.on("/update", configUpdateHandler);
  server.begin();

  while (1) {
    server.handleClient();
  }
}
