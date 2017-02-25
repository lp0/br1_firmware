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

#define MAX_PIXELS 1000

#define HUE_MAX 360
#define HUE_EXP_RANGE 60
#define HUE_EXP_TIMES 2
#define HUE_EXP_MAX ((HUE_MAX - HUE_EXP_RANGE) + (HUE_EXP_TIMES * HUE_EXP_RANGE))

#define MAX_BURSTS 5

const uint8_t irPin = 14;
const uint8_t buttonPin = 13;
const uint8_t dataPin = 2;
const unsigned int udpPort = 2812;

const char *wifiModeNames[] = { "STA", "AP", NULL };

const char *colourOrderNames[] = { "RGB", "GRB", "BRG", NULL };
const uint16_t colourOrderValues[] = { NEO_RGB, NEO_GRB, NEO_BRG };

const char *modeNames[255] = { "Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White",
  "Hue Scroll (1x, slow)", "Hue Scroll (1x, medium)", "Hue Scroll (1x, fast)",
  "Hue Scroll (2x, slow)", "Hue Scroll (2x, medium)", "Hue Scroll (2x, fast)",
  "Hue Fade", "Christmas (Red and Green)", "Christmas (Red and Green (Twinkle)",
  "White (Twinkle)", "Christmas (Work)", "Christmas (Work (Twinkle))",
  "Hue Scroll (1x, slow (Twinkle))", "Hue Scroll (1x, medium (Twinkle))", "Hue Scroll (1x, fast (Twinkle))",
  "Hue Scroll (2x, slow (Twinkle))", "Hue Scroll (2x, medium (Twinkle))", "Hue Scroll (2x, fast (Twinkle))",
  "Hue Static (Twinkle)", "Hue Fade (Twinkle)", "Knight Rider", "Knight Rider (Hue Fade)",
  "Single Random 1 (slow)", "Single Random 1 (medium)", "Single Random 1 (fast)", "Full Random 1 (slow)", "Full Random 1 (medium)", "Full Random 1 (fast)",
  "Single Random 2 (slow)", "Single Random 2 (medium)", "Single Random 2 (fast)", "Full Random 2 (slow)", "Full Random 2 (medium)", "Full Random 2 (fast)",
  "Burst  (1x, 7 colours)", "Burst  (1x, 10 colours)", "Burst  (1x, 14 colours)", "Burst  (1x, 20 colours)",
  "Burst  (2x, 7 colours)", "Burst  (2x, 10 colours)", "Burst  (2x, 14 colours)", "Burst  (2x, 20 colours)",
  "Burst  (3x, 7 colours)", "Burst  (3x, 10 colours)", "Burst  (3x, 14 colours)", "Burst  (3x, 20 colours)",
  "Burst  (4x, 7 colours)", "Burst  (4x, 10 colours)", "Burst  (4x, 14 colours)", "Burst  (4x, 20 colours)",
  "Burst  (5x, 7 colours)", "Burst  (5x, 10 colours)", "Burst  (5x, 14 colours)", "Burst  (5x, 20 colours)",
  "Rainbow Twinkle",
  NULL };

struct EepromData {
  uint8_t configured;
  char ssid[128];
  char passphrase[128];
  uint16_t pixelcount;
  uint16_t colourorder;
  uint8_t scale0red;
  uint8_t scale0green;
  uint8_t scale0blue;
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
boolean ledModeChanged = true;
uint8_t buttonState = HIGH;

uint8_t scalered[MAX_PIXELS];
uint8_t scalegreen[MAX_PIXELS];
uint8_t scaleblue[MAX_PIXELS];
uint8_t multiplier;

void configuration_mode();
void run_mode();
void udpLoop();
void tcpLoop();
void ledLoop();
void singleColour(uint8_t red, uint8_t green, uint8_t blue);
void makeScale();

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
    Serial.print(ledMode, DEC);
    if (modeNames[ledMode]) {
      Serial.print(" ");
      Serial.println(modeNames[ledMode]);
    } else {
      Serial.println();
    }
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

void makeScale() {
  for (int i = 0; i < MAX_PIXELS; i++) {
    scalered[i] = eepromData.scale0red;
    scalegreen[i] = eepromData.scale0green;
    scaleblue[i] = eepromData.scale0blue;
  }

  multiplier = eepromData.pixelcount / 50;
  if (multiplier == 0)
    multiplier = 1;
}

void singleColour(uint8_t red, uint8_t green, uint8_t blue) {

  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, RgbColor(red * scalered[i] / 255,
                                         green * scalegreen[i] / 255,
                                         blue * scaleblue[i] / 255));
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
RgbColor ledHSV(int h, double s, double v, int pos, bool hdr) {
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

  if (hdr) {
    return RgbColor(red * scalered[pos] / 255,
                        green * scalegreen[pos] / 255,
                        blue * scaleblue[pos] / 255);
  } else {
    return RgbColor(red * scalered[pos] / 255,
                        green * scalegreen[pos] / 255,
                        blue * scaleblue[pos] / 255);
  }
}

RgbColor ledExpHSV(int h, double s, double v, int pos) {
  return ledExpHSV(h, s, v, pos, false);
}

RgbColor ledExpHSV(int h, double s, double v, int pos, bool hdr) {
  if (h < (HUE_EXP_TIMES * HUE_EXP_RANGE))
    return ledHSV(h / HUE_EXP_TIMES, s, v, pos, hdr);
  else
    return ledHSV(h - ((HUE_EXP_TIMES - 1) * HUE_EXP_RANGE), s, v, pos, hdr);
}

void hsvFade() {
  static int hue = 0;
  static unsigned long lastChange = 0;
  unsigned long interval = 50;

  if (millis() - lastChange > interval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      pixels.SetPixelColor(i, ledExpHSV(hue, 1.0, 1.0, i));
    }
    pixels.Show();
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
  }
}

boolean hsvFadeNoShow() {
  static int hue = 0;
  static unsigned long lastChange = 0;
  unsigned long interval = 50;

  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, ledExpHSV(hue, 1.0, 1.0, i));
  }

  if (millis() - lastChange > interval) {
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
    return true;
  }
  return false;
}

boolean hsvStaticNoShow() {
  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, ledExpHSV(i * HUE_EXP_MAX / eepromData.pixelcount, 1.0, 1.0, i));
  }
  return false;
}

void hsvStatic() {
  hsvStaticNoShow();
  pixels.Show();
}

void hsvScroll(unsigned int repeat, unsigned long interval) {
  static int hue = 0;
  static unsigned long lastChange = 0;

  if (millis() - lastChange > interval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      pixels.SetPixelColor(i, ledExpHSV(((repeat * i * HUE_EXP_MAX / eepromData.pixelcount) + hue) % HUE_EXP_MAX, 1.0, 1.0, i));
    }
    pixels.Show();
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
  }
}


unsigned int hsv_scroll_repeat;
unsigned long hsv_scroll_interval;

boolean hsvScrollNoShow() {
  static int hue = 0;
  static unsigned long lastChange = 0;

  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, ledExpHSV(((multiplier * i * HUE_EXP_MAX / eepromData.pixelcount) + hue) % HUE_EXP_MAX, 1.0, 1.0, i));
  }

  if (millis() - lastChange > hsv_scroll_interval) {
    hue++;
    hue %= HUE_EXP_MAX;
    lastChange = millis();
    return true;
  }
  return false;
}

static RgbColor christmasRGValues[MAX_PIXELS];

boolean christmasRedAndGreen(boolean show) {
  static unsigned long lastChange = 0;
  unsigned long interval = (show ? 200 : 1000) / multiplier;

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++) {
      const RgbColor red = RgbColor(scalered[i], 0, 0);
      const RgbColor green = RgbColor(0, scalegreen[i], 0);
     
      if (random(0, 2) == 0) {
        christmasRGValues[i] = red;
        pixels.SetPixelColor(i, red);
      } else {
        christmasRGValues[i] = green;
        pixels.SetPixelColor(i, green);
      }
    }
    if (show)
      pixels.Show();
    ledModeChanged = false;
    lastChange = millis();
    return true;
  }

  if (millis() - lastChange > interval) {
    int i = random(0, eepromData.pixelcount);
    const RgbColor red = RgbColor(scalered[i], 0, 0);
    const RgbColor green = RgbColor(0, scalegreen[i], 0);

    if (christmasRGValues[i] == green) {
      christmasRGValues[i] = red;
      if (show)
        pixels.SetPixelColor(i, red);
    } else {
      christmasRGValues[i] = green;
      if (show)
        pixels.SetPixelColor(i, green);
    }
    if (show) {
      pixels.Show();
    } else {
      for (int i = 0; i < eepromData.pixelcount; i++)
        pixels.SetPixelColor(i, christmasRGValues[i]);
    }
    lastChange = millis();
    return true;
  }

  if (!show)
    for (int i = 0; i < eepromData.pixelcount; i++)
      pixels.SetPixelColor(i, christmasRGValues[i]);
  return false;
}

void christmasRedAndGreen() {
  christmasRedAndGreen(true);
}

boolean christmasRedAndGreenNoShow() {
  return christmasRedAndGreen(false);
}

uint8_t applyTwinkleLevelC(uint8_t value, uint8_t level, uint8_t scale) {
  level = (255 - level);
  value = (value * 255 / scale);
  if (value >= level) {
    value = (value - level) * scale / 255;
  } else {
    value = 0;
  }
  return value;
}

void applyTwinkleLevel(int i, uint8_t level) {
  RgbColor tmp;
  HsbColor hsb;

  tmp = pixels.GetPixelColor(i);

#if 0
  tmp.R = applyTwinkleLevelC(tmp.R, level, scalered[i]);
  tmp.G = applyTwinkleLevelC(tmp.G, level, scalegreen[i]);
  tmp.B = applyTwinkleLevelC(tmp.B, level, scaleblue[i]);
#endif

  hsb = HsbColor(tmp);
  hsb.B *= level / 255.0f;
  tmp = RgbColor(hsb);

  pixels.SetPixelColor(i, tmp);
}

void twinkle(boolean (*func)(void), uint8_t low, uint8_t high, uint8_t step, boolean invert) {
  static unsigned long lastChange = 0;
  static unsigned long lastPulse = 0;
  static uint8_t levels[MAX_PIXELS];
  static uint8_t current[MAX_PIXELS];
  unsigned long pulseInterval = 250 / multiplier;
  unsigned long changeInterval = 10;
  boolean refresh = false;

  if (ledModeChanged) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      levels[i] = invert ? high : low;
      current[i] = 0;
    }
    func();
    refresh = true;
    lastChange = millis();
    lastPulse = millis();
    ledModeChanged = false;
  }

  if (millis() - lastPulse > pulseInterval) {
    for (int i = 0; i < 10; i++) {
      int target = random(0, eepromData.pixelcount);
      if (current[target] == 0 && levels[target] == (invert ? high : low)) {
        current[target] = 1;
        break;
      }
    }
    lastPulse = millis();
  }

  if (millis() - lastChange > changeInterval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      if (current[i] == 1) {
        // this pixel is rising
        if (invert ? (levels[i] - step < low) : (levels[i] + step > high)) {
          // the pixel has reached maximum
          levels[i] = invert ? low : high;
          current[i] = 0;
        } else {
          levels[i] = invert ? (levels[i] - step) : (levels[i] + step);
        }
      } else if (invert ? (levels[i] < high) : (levels[i] > low)) {
        // this pixel is falling
        if (invert ? (levels[i] + step > high) : (levels[i] - step < low)) {
          levels[i] = invert ? high : low;
        } else {
          levels[i] = invert ? (levels[i] + step) : (levels[i] - step);
        }
      }
    }
    lastChange = millis();
    refresh = true;
  }

  if (func() || refresh) {
    for (int i = 0; i < eepromData.pixelcount; i++)
      applyTwinkleLevel(i, levels[i]);
    pixels.Show();
  }
}

void christmasRedAndGreenTwinkle() {
  twinkle(christmasRedAndGreenNoShow, 50, 255, 10, true);
}

boolean whiteNoShow() {
  for (int i = 0; i < eepromData.pixelcount; i++) {
    pixels.SetPixelColor(i, RgbColor(scalered[i],
                                         scalegreen[i],
                                         scaleblue[i]));
  }
  return false;
}

void whiteTwinkle() {
  twinkle(whiteNoShow, 50, 255, 10, false);
}

void hsvScrollTwinkle(unsigned int repeat, unsigned long interval) {
  hsv_scroll_repeat = repeat;
  hsv_scroll_interval = interval;
  twinkle(hsvScrollNoShow, 50, 255, 10, true);
}

void hsvStaticTwinkle() {
  twinkle(hsvStaticNoShow, 50, 255, 10, true);
}

void hsvFadeTwinkle() {
  twinkle(hsvFadeNoShow, 50, 255, 10, true);
}

static RgbColor christmasWorkValues[MAX_PIXELS];

boolean christmasWork(boolean show) {
  static unsigned long lastChange = 0;
  unsigned long interval = (show ? 200 : 1000) / multiplier;

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++) {
      const RgbColor grey = RgbColor(74 * scalered[i] / 255, 79 * scalegreen[i] / 255, 85 * scaleblue[i] / 255);
      const RgbColor red = RgbColor(194 * scalered[i] / 255, 4 * scalegreen[i] / 255, 24 * scaleblue[i] / 255);
      const RgbColor lightBlue = RgbColor(0, 195 * scalegreen[i] / 255, 215 * scaleblue[i] / 255);
      const RgbColor darkBlue = RgbColor(0, 51 * scalegreen[i] / 255, 161 * scaleblue[i] / 255);
      const RgbColor colours[4] = { grey, red, lightBlue, darkBlue };
      const int num_colours = sizeof(colours)/sizeof(RgbColor);

      christmasWorkValues[i] = colours[random(0, num_colours)];
      pixels.SetPixelColor(i, christmasWorkValues[i]);
    }
    if (show)
      pixels.Show();
    ledModeChanged = false;
    lastChange = millis();
    return true;
  }

  if (millis() - lastChange > interval) {
    int i = random(0, eepromData.pixelcount);
    const RgbColor grey = RgbColor(74 * scalered[i] / 255, 79 * scalegreen[i] / 255, 85 * scaleblue[i] / 255);
    const RgbColor red = RgbColor(194 * scalered[i] / 255, 4 * scalegreen[i] / 255, 24 * scaleblue[i] / 255);
    const RgbColor lightBlue = RgbColor(0, 195 * scalegreen[i] / 255, 215 * scaleblue[i] / 255);
    const RgbColor darkBlue = RgbColor(0, 51 * scalegreen[i] / 255, 161 * scaleblue[i] / 255);
    const RgbColor colours[4] = { grey, red, lightBlue, darkBlue };
    const int num_colours = sizeof(colours)/sizeof(RgbColor);
    int n = random(1, num_colours);

    for (int j = 0; j < num_colours; j++) {
      if (christmasWorkValues[i] == colours[j]) {
        christmasWorkValues[i] = colours[(j + n) % num_colours];
        if (show)
          pixels.SetPixelColor(i, christmasWorkValues[i]);
        break;
      }
    }
    if (show) {
      pixels.Show();
    } else {
      for (int i = 0; i < eepromData.pixelcount; i++)
        pixels.SetPixelColor(i, christmasWorkValues[i]);
    }
    lastChange = millis();
    return true;
  }

  if (!show)
    for (int i = 0; i < eepromData.pixelcount; i++)
      pixels.SetPixelColor(i, christmasWorkValues[i]);
  return false;
}

void christmasWork() {
  christmasWork(true);
}

boolean christmasWorkNoShow() {
  return christmasWork(false);
}

void christmasWorkTwinkle() {
  twinkle(christmasWorkNoShow, 50, 255, 10, true);
}

void knightRider(boolean hsvFade) {
  static unsigned long lastChange = 0;
  static int pos = 0;
  static int dir = 1;
  unsigned long interval = 600 / eepromData.pixelcount;
  const uint8_t active = 12; // percentage fully bright
  const uint8_t fade = 16; // percentage to fade off at each end
  const float fadeRate = 0.75f;
  int pActive = eepromData.pixelcount * active / 100 / 2;
  int pFade = eepromData.pixelcount * fade / 100;

  static int hue = 0;
  static unsigned long lastHueChange = 0;
  unsigned long hueInterval = 50;

  boolean change = false;
  boolean refresh = false;

  if (pActive < 1)
    pActive = 1;
  if (pFade < 1)
    pFade = 1;

  if (ledModeChanged) {
    pos = pActive;
    dir = 1;
    lastChange = millis() - interval - 1;

    hue = 0;
    lastHueChange = millis();

    ledModeChanged = false;
  }

  if (hsvFade) {
    if (millis() - lastHueChange > hueInterval) {
      hue++;
      hue %= HUE_EXP_MAX;
      lastHueChange = millis();
      refresh = true;
    }
  }

  if (millis() - lastChange > interval) {
    change = true;
    refresh = true;
  }

  if (refresh) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      RgbColor base;
      HsbColor tmp;

      if (hsvFade) {
        base = ledExpHSV(hue, 1.0, 1.0, i);
      } else {
        base = RgbColor(scalered[i], 16 * scalegreen[i] / 255, 0);
      }

      tmp = HsbColor(base);

      if (i >= pos - pActive && i <= pos + pActive) {
        // full brightness
      } else if (i >= pos - pActive - pFade && i <= pos + pActive + pFade) {
        if (i < pos) {
          //tmp.B *= (float)(pFade - ((pos - pActive) - i)) / (float)pFade * 0.5f;
          tmp.B *= 0.5f;
          for (int j = ((pos - pActive) - i); j > 0; j--)
            tmp.B *= fadeRate;
        } else {
          //tmp.B *= (float)(pFade - (i - (pos + pActive))) / (float)pFade * 0.5f;
          tmp.B *= 0.5f;
          for (int j = (i - (pos + pActive)); j > 0; j--)
            tmp.B *= fadeRate;
        }
      } else {
        tmp.B = 0;
      }

      pixels.SetPixelColor(i, tmp);
    }

    pixels.Show();
 }

 if (change) {
    pos += dir;
    if (pos > eepromData.pixelcount - pActive) {
      dir = -1;
      pos--;
    } else if (pos < pActive) {
      dir = 1;
      pos++;
    }
    lastChange = millis();
  }
}

void burst(unsigned int bursts, unsigned int colours) {
  static unsigned long lastChange = 0;
  static int pos[MAX_BURSTS] = { 0 };
  static int hue[MAX_BURSTS] = { 0 };
  static bool active[MAX_BURSTS] = { false };
  const unsigned long interval = 600 / eepromData.pixelcount;
  const float fadeRate = 0.75f;
  int pActive = eepromData.pixelcount / (bursts + 2) / 3;
  int pFade = eepromData.pixelcount / (bursts + 2) / 6;
  int pSide;

  boolean change = false;
  boolean refresh = false;

  if (bursts > MAX_BURSTS)
    return;

  if (pActive < 1)
    pActive = 1;
  if (pFade < 1)
    pFade = 1;
  pSide = pActive + pFade;

  if (ledModeChanged) {
    for (int i = 0; i < bursts; i++) {
      active[i] = false;
      pos[i] = 0;
    }
    lastChange = millis() - interval - 1;

    active[0] = true;
    hue[0] = 0;
    for (int i = 1; i < bursts; i++)
      hue[i] = (hue[i - 1] + (HUE_EXP_MAX / colours)) % HUE_EXP_MAX;

    ledModeChanged = false;
  }

  if (millis() - lastChange > interval) {
    change = true;
    refresh = true;
  }

  if (refresh) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      HsbColor set = HsbColor(RgbColor(0, 0, 0));

      for (int burst = 0; burst < bursts; burst++) {
        RgbColor base = ledExpHSV(hue[burst], 1.0, 1.0, i);
        HsbColor tmp = HsbColor(base);

        if (!active[burst]) {
          continue;
        } else if (i >= pos[burst] - pActive && i <= pos[burst] + pActive) {
          // full brightness
        } else if (i >= pos[burst] - pActive - pFade && i <= pos[burst] + pActive + pFade) {
          if (i < pos[burst]) {
            tmp.B *= 0.5f;
            for (int j = ((pos[burst] - pActive) - i); j > 0; j--)
              tmp.B *= fadeRate;
          } else {
            tmp.B *= 0.5f;
            for (int j = (i - (pos[burst] + pActive)); j > 0; j--)
              tmp.B *= fadeRate;
          }
        } else {
          continue;
        }

        set = tmp;
        break;
      }

      pixels.SetPixelColor(i, set);
    }

    pixels.Show();
  }

  if (change) {
    for (int i = 0; i < bursts; i++) {
      if (active[i]) {
        if (pos[i] >= eepromData.pixelcount + pSide) {
          pos[i] = 0 - pSide;
          hue[i] = (hue[i] + (HUE_EXP_MAX / colours) * bursts) % HUE_EXP_MAX;
          Serial.print("Burst ");
          Serial.print(i, DEC);
          Serial.print(" restarted, pos[] = { ");
          for (int j = 0; j < bursts; j++) {
              if (j > 0)
                Serial.print(", ");
            Serial.print(pos[j], DEC);
            if (active[i])
              Serial.print("*");
            else
              Serial.print(".");
          }
          Serial.println(" }");
        } else {
          pos[i]++;
        }
      } else if (i > 0 && active[i - 1] && pos[i - 1] >= (int)((eepromData.pixelcount / bursts) - pSide)) {
        pos[i] = 0 - pSide;
        active[i] = true;
      }
    }
    lastChange = millis();
  }
}

RgbColor makeRandom1(int pos) {
  RgbColor tmp = RgbColor(HslColor(random(0, 256) / 255.0f, random(128, 256) / 255.0f, random(64, 128) / 255.0f));
  tmp.R = tmp.R * scalered[pos] / 255;
  tmp.G = tmp.G * scalegreen[pos] / 255;
  tmp.B = tmp.B * scaleblue[pos] / 255;
  return tmp;
}

RgbColor makeRandom2(int pos) {
  return ledExpHSV(random(0, HUE_EXP_MAX), 1.0, 1.0, pos);
}

void random_single(RgbColor (*makeRandom)(int), unsigned long interval) {
  static unsigned long lastChange = 0;

  interval /= multiplier;

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++)
      pixels.SetPixelColor(i, makeRandom(i));
    pixels.Show();
    ledModeChanged = false;
  }

  if (millis() - lastChange > interval) {
    int i = random(0, eepromData.pixelcount);
    pixels.SetPixelColor(i, makeRandom(i));
    pixels.Show();
    lastChange = millis();
  }
}

void random_full(RgbColor (*makeRandom)(int), unsigned long interval) {
  static unsigned long lastChange = 0;

  if (ledModeChanged) {
    // randomise all of the pixels
    for (int i = 0; i < eepromData.pixelcount; i++)
      pixels.SetPixelColor(i, makeRandom(i));
    pixels.Show();
    ledModeChanged = false;
  }

  if (millis() - lastChange > interval) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
         pixels.SetPixelColor(i, makeRandom(i));
    }
    pixels.Show();
    lastChange = millis();
  }
}

void rainbow_twinkle(unsigned long interval, uint8_t low, uint8_t high, uint8_t step) {
  static unsigned long lastChange = 0;
  static unsigned long lastPulse = 0;
  static uint8_t levels[MAX_PIXELS];
  static uint8_t current[MAX_PIXELS];
  static uint16_t hues[MAX_PIXELS];
  unsigned long pulseInterval = interval / multiplier;
  unsigned long changeInterval = 10;
  boolean refresh = false;

  if (ledModeChanged) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      levels[i] = low;
      current[i] = 0;
      hues[i] = 0;
    }
    refresh = true;
    lastChange = millis();
    lastPulse = millis();
    ledModeChanged = false;
  }

  if (millis() - lastPulse > pulseInterval) {
    for (int i = 0; i < 10; i++) {
      int target = random(0, eepromData.pixelcount);
      if (current[target] == 0 && levels[target] == low) {
        current[target] = 1;
        hues[target] = random(0, HUE_EXP_MAX);
        break;
      }
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
    }
    lastChange = millis();
    refresh = true;
  }

  if (refresh) {
    for (int i = 0; i < eepromData.pixelcount; i++) {
      double s;
      double v;

      s = (double)(levels[i] - low) / (double)(high - low);
      v = levels[i] / 255.0f;

      pixels.SetPixelColor(i, ledExpHSV(hues[i], s, v, i, true));
    }
    pixels.Show();
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
    // green
    singleColour(0, 255, 0);
    break;
  case 3:
    // yellow
    singleColour(255, 255, 0);
    break;
  case 4:
    // blue
    singleColour(0, 0, 255);
    break;
  case 5:
    // magenta
    singleColour(255, 0, 255);
    break;
  case 6:
    // cyan
    singleColour(0, 255, 255);
    break;
  case 7:
    // white
    singleColour(255, 255, 255);
    break;
  case 8:
    hsvScroll(1, 50);
    break;
  case 9:
    hsvScroll(1, 20);
    break;
  case 10:
    hsvScroll(1, 5);
    break;
  case 11:
    hsvScroll(2, 50);
    break;
  case 12:
    hsvScroll(2, 20);
    break;
  case 13:
    hsvScroll(2, 5);
    break;
  case 14:
    hsvFade();
    break;
  case 15:
    christmasRedAndGreen();
    break;
  case 16:
    christmasRedAndGreenTwinkle();
    break;
  case 17:
    whiteTwinkle();
    break;
  case 18:
    christmasWork();
    break;
  case 19:
    christmasWorkTwinkle();
    break;
  case 20:
    hsvScrollTwinkle(1, 50);
    break;
  case 21:
    hsvScrollTwinkle(1, 20);
    break;
  case 22:
    hsvScrollTwinkle(1, 5);
    break;
  case 23:
    hsvScrollTwinkle(2, 50);
    break;
  case 24:
    hsvScrollTwinkle(2, 20);
    break;
  case 25:
    hsvScrollTwinkle(2, 5);
    break;
  case 26:
    hsvStaticTwinkle();
    break;
  case 27:
    hsvFadeTwinkle();
    break;
  case 28:
    knightRider(false);
    break;
  case 29:
    knightRider(true);
    break;
  case 30:
    random_single(makeRandom1, 200);
    break;
  case 31:
    random_single(makeRandom1, 150);
    break;
  case 32:
    random_single(makeRandom1, 100);
    break;
  case 33:
    random_full(makeRandom1, 1000);
    break;
  case 34:
    random_full(makeRandom1, 500);
    break;
  case 35:
    random_full(makeRandom1, 200);
    break;
  case 36:
    random_single(makeRandom2, 200);
    break;
  case 37:
    random_single(makeRandom2, 150);
    break;
  case 38:
    random_single(makeRandom2, 100);
    break;
  case 39:
    random_full(makeRandom2, 1000);
    break;
  case 40:
    random_full(makeRandom2, 500);
    break;
  case 41:
    random_full(makeRandom2, 200);
    break;
  case 42:
    burst(1, 7);
    break;
  case 43:
    burst(1, 10);
    break;
  case 44:
    burst(1, 14);
    break;
  case 45:
    burst(1, 20);
    break;
  case 46:
    burst(2, 7);
    break;
  case 47:
    burst(2, 10);
    break;
  case 48:
    burst(2, 14);
    break;
  case 49:
    burst(2, 20);
    break;
  case 50:
    burst(3, 7);
    break;
  case 51:
    burst(3, 10);
    break;
  case 52:
    burst(3, 14);
    break;
  case 53:
    burst(3, 20);
    break;
  case 54:
    burst(4, 7);
    break;
  case 55:
    burst(4, 10);
    break;
  case 56:
    burst(4, 14);
    break;
  case 57:
    burst(4, 20);
    break;
  case 58:
    burst(5, 7);
    break;
  case 59:
    burst(5, 10);
    break;
  case 60:
    burst(5, 14);
    break;
  case 61:
    burst(5, 20);
    break;
  case 62:
    rainbow_twinkle(100, 50, 255, 5);
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

  form.reserve(3072);
  form += "<!DOCTYPE html>"
      "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<style type=\"text/css\">"
      "html, body, form { padding: 0; margin: 0; }"
      "input {"
        "border: none; background-color: #99f; padding: 1em 0 1em 0;"
        "border-bottom: 2px solid #fff;"
        "margin: 0;"
        "width: 100%;"
        "font-weight: bold;"
      "}"
      "input:active {"
        "background-color: #f99;"
      "}"
      "#selected {"
        "background-color: #9f9;"
      "}"
      "</style>"
      "<script>"
      "Element.prototype.documentOffsetTop = function () {"
        "return this.offsetTop + (this.offsetParent ? this.offsetParent.documentOffsetTop() : 0);"
      "};"
      "window.onload = function() {"
        "var selected = document.getElementById('selected');"
        "if (selected) {"
          "var top = selected.documentOffsetTop() - (window.innerHeight / 2);"
          "window.scrollTo(0, top);"
        "}"
      "};"
      "</script>"
      "</head><body>"
      "<form method=\"POST\" action=\"apply\">";

  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "default") {
      form += "<input type=\"hidden\" name=\"default\" value=\"1\"/>";
    }
  }

  for (i = 0; modeNames[i] != NULL; i++) {
    form += "<input type=\"submit\" name=\"ledmode_";
    form += i;
    form += "\"";
    if (ledMode == i)
      form += " id=\"selected\"";
    form += " value=\"";
    form += modeNames[i];
    form += "\"><br>";
  }

  form += "</form>";

  server.send(200, "text/html", form);
}

void runUpdateHandler() {
  int setDefault = 0;

  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "default") {
      setDefault = 1;
    }

    if (server.argName(i).startsWith("ledmode_")) {
      ledMode = server.argName(i).substring(8).toInt();
      ledModeChanged = true;
      Serial.print("Updated mode=");
      Serial.println(ledMode, DEC);
    }
  }

  if (setDefault) {
    eepromData.defaultmode = ledMode;
    Serial.print("Updated default mode=");
    Serial.println(ledMode, DEC);
    EEPROM.put(0, eepromData);
    EEPROM.commit();

    server.send(200, "text/html", "<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"2;URL=/\"></head><p>Updated default</p>");
  } else {
    String redirect;
    redirect.reserve(128);
    redirect += "HTTP/1.1 302 Updated\r\nLocation: http://";
    if (eepromData.wifimode == 1) {
      redirect += WiFi.softAPIP().toString();
    } else {
      redirect += WiFi.localIP().toString();
    }
    redirect += "/\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    server.sendContent(redirect);
  }
}

void runConfigHandler() {
  String form;

  form.reserve(3072);
  form += "<!DOCTYPE html>"
      "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
      "<form method=\"POST\" action=\"apply2\">";

  form += "Scaling: "
      "R<input type=\"number\" name=\"s0r\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scale0red;
  form += "\"/>"
      "G<input type=\"number\" name=\"s0g\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scale0green;
  form += "\"/>"
      "B<input type=\"number\" name=\"s0b\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.scale0blue;
  form += "\"/><br/>";

  form += "<br/>";
  form += "Pixels: <input type=\"number\" name=\"pixelcount\" min=\"1\" value=\"";
  form += eepromData.pixelcount;
  form += "\"/><br/>";
  form += "Default Mode: <input type=\"number\" name=\"defaultmode\" min=\"0\" max=\"255\" value=\"";
  form += eepromData.defaultmode;
  form += "\"/><br/>";
  form += "<input type=\"submit\" /></form>";

  server.send(200, "text/html", form);
}

void runConfigUpdateHandler() {
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "s0r") {
      eepromData.scale0red = server.arg(i).toInt();
    }
    if (server.argName(i) == "s0g") {
      eepromData.scale0green = server.arg(i).toInt();
    }
    if (server.argName(i) == "s0b") {
      eepromData.scale0blue = server.arg(i).toInt();
    }
    if (server.argName(i) == "pixelcount") {
      eepromData.pixelcount = server.arg(i).toInt() % MAX_PIXELS;
    }
    if (server.argName(i) == "defaultmode") {
      eepromData.defaultmode = server.arg(i).toInt();
    }
  }
  makeScale();
  pixels.setPixelCount(eepromData.pixelcount);
  ledModeChanged = true;
  Serial.println("Updated config:");
  Serial.print("pixelcount=");
  Serial.println(eepromData.pixelcount, DEC);
  Serial.print("defaultmode=");
  Serial.println(eepromData.defaultmode, DEC);
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
    //eepromData.wifimode = 0;
    switch (eepromData.wifimode) {
    case 1:
      // go into access point mode
      WiFi.mode(WIFI_AP);
      if (strlen(eepromData.passphrase) > 0)
        WiFi.softAP(eepromData.ssid, eepromData.passphrase);
      else
        WiFi.softAP(eepromData.ssid);
      { uint8 mode = 0; wifi_softap_set_dhcps_offer_option(OFFER_ROUTER, &mode); }
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

    makeScale();
    pixels.setPixelCount(eepromData.pixelcount);
    pixels.setType(eepromData.colourorder | NEO_KHZ800 | NEO_IRQLOCK);
    pixels.Begin();

    Udp.begin(udpPort);
    server.on("/", runRootHandler);
    server.on("/apply", runUpdateHandler);
    server.on("/config", runConfigHandler);
    server.on("/apply2", runConfigUpdateHandler);
    server.on("/uptime", runUptimeHandler);
    server.begin();
    ledMode = eepromData.defaultmode;
    ledModeChanged = true;
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
      "R<input type=\"number\" name=\"s0r\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scale0red : 255);
  form += "\"/>"
      "G<input type=\"number\" name=\"s0g\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scale0green : 255);
  form += "\"/>"
      "B<input type=\"number\" name=\"s0b\" min=\"0\" max=\"255\" value=\"";
  form += (eepromData.configured == 1 ? eepromData.scale0blue : 255);
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
      eepromData.pixelcount = server.arg(i).toInt() % MAX_PIXELS;
    }
    if (server.argName(i) == "colourorder") {
      eepromData.colourorder = server.arg(i).toInt();
    }
    if (server.argName(i) == "s0r") {
      eepromData.scale0red = server.arg(i).toInt();
    }
    if (server.argName(i) == "s0g") {
      eepromData.scale0green = server.arg(i).toInt();
    }
    if (server.argName(i) == "s0b") {
      eepromData.scale0blue = server.arg(i).toInt();
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
  makeScale();
  pixels.setPixelCount(eepromData.pixelcount);
  pixels.setType(eepromData.colourorder | NEO_KHZ800);
  pixels.Begin();
  singleColour(0, 0, 0);
  pixels.SetPixelColor(0, RgbColor(eepromData.scale0red, 0, 0));
  pixels.SetPixelColor(1, RgbColor(0, eepromData.scale0green, 0));
  pixels.SetPixelColor(2, RgbColor(0, 0, eepromData.scale0blue));
  pixels.SetPixelColor(eepromData.pixelcount - 1,
    RgbColor(eepromData.scale0red, eepromData.scale0green, eepromData.scale0blue));
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
  { uint8 mode = 0; wifi_softap_set_dhcps_offer_option(OFFER_ROUTER, &mode); }
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
