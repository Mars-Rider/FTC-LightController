#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------------- CONFIG ----------------
#define MAX_CHANNELS 5
#define LEDS_PER_CHANNEL 300
#define I2C_ADDR 0x08
#define MIN_FPS 30

// Pins
#define PIN_SCL_CLOCK 5 // D1: I2C SCL or Digital Clock
#define PIN_SDA_DATA 4  // D2: I2C SDA or Digital Data
#define PIN_PWM 15      // D5: PWM Input (Updated to 14 for Wemos D1/NodeMCU D5)

// Interface Modes
enum InputMode
{
  MODE_UNKNOWN,
  MODE_I2C,
  MODE_DIGITAL,
  MODE_PWM
};
InputMode currentMode = MODE_UNKNOWN;

// Global Settings
byte quality = 100;
float maxCurrent = 2.0f; // 2000 ma

// ---------------- DATA STRUCTURES ----------------
struct LEDChannel
{
  uint8_t pin = 0;
  unsigned int numLEDs = LEDS_PER_CHANNEL;

  CRGB *leds = nullptr;

  // Colors
  int h = 0;
  uint8_t s = 255;
  int brightness = 255;

  // Parameters
  bool rainbow = false;
  int colorPeriod = 10000;
  int hueMin = 0;
  int hueMax = 0;
  uint8_t pattern = 1;
  int trailLength = 4;
  int sourceCenter = 0;
  int sourceLength = 4;
  bool gradient = false;
  bool moving = false;
  int period = 500;
  bool direction = true;

  unsigned long lastUpdate = millis();
  int pos[2] = {0, 0};
};

// ---------------- CHANNELS ----------------
LEDChannel channels[MAX_CHANNELS];
int numChannels = 0;

// ---------------- WEBSOCKET VARS ----------------
ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
bool wifi = false;
bool clientConnected = false;

// ---------------- DIGITAL PROTOCOL VARS ----------------
volatile byte digBuffer[64];
volatile int digBitIdx = 0;
volatile int digByteIdx = 0;
volatile byte digCurrByte = 0;
volatile unsigned long lastDigTime = 0;

// ---------------- PWM PROTOCOL VARS ----------------
// Math Constants for 500-2500us range
const int MIN_PULSE = 400;
const int MAX_PULSE = 2600;
const int BASE_OFFSET = 500;
const float STEP_SIZE = 133.33;

// Buffer State
byte pwmBuffer[64]; // Not volatile because we fill it in loop(), not interrupt
int pwmIdx = 0;
bool highNibbleReceived = false;
byte highNibble = 0;
int bytesExpected = 0;
bool receiving = false;
volatile bool pwmActiveDetected = false; // Flag from interrupt

// ---------------- SWITCHING LOGIC VARS ----------------
unsigned long sclLowStartTime = 0; // To detect Digital Idle State

// ---------------- FORWARD DECLARATIONS ----------------
void applyPattern(LEDChannel &ch);
void updateLEDs();
CRGB normalizeLEDs(CRGB c);
void i2cReceiveEvent(int bytesReceived);
void checkDigitalInput();
void checkPWMInput();
void handleNibble(int val);
void checkInterfaceAutoSwitch();
void switchMode(InputMode newMode);
void processPacket(uint8_t *data, int length);
void handleSettingsPacket(uint8_t *data);
void handleStartPacket(uint8_t *data);
void handleStripPacket(uint8_t *data);
void handleWiFiPacket(uint8_t *data);
void handleWebSocket();
void startWebSocket(const char *ssid);
void trimSSID(char *str);
bool handleFileRead(String path);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
IRAM_ATTR void onDigitalClockRise();
IRAM_ATTR void onPWMChange(); // Only used to detect activity

// ---------------- SETUP ----------------
void setup()
{
  Serial.begin(115200);
  delay(500);

  // 1. Setup Pins
  channels[0].pin = 13; // D7
  channels[1].pin = 12; // D6
  channels[2].pin = 14; // D5 (Shared PWM)
  channels[3].pin = 2;  // D4
  channels[4].pin = 16; // D0

  for (LEDChannel &channel : channels)
  {
    channel.leds = new CRGB[LEDS_PER_CHANNEL];
  }

  Serial.println("System Booted.");

  // ---------------- INITIAL MODE ----------------
  // We default to I2C because it is "Passive".
  switchMode(MODE_I2C);

  // Always enable PWM interrupt to "Listen" for pulses in the background
  pinMode(PIN_PWM, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PWM), onPWMChange, CHANGE);
}

unsigned long lastMillis = 0;
double fps;

// ---------------- MAIN LOOP ----------------
void loop()
{
  unsigned long currMillis = millis();

  // 1. Check for Signals to Auto-Switch Mode
  checkInterfaceAutoSwitch();

  // 2. Poll Inputs based on Current Mode
  if (currentMode == MODE_DIGITAL)
    checkDigitalInput();

  // PWM check runs if in PWM or Idle I2C mode (to listen for wake-up)
  if (currentMode == MODE_PWM || currentMode == MODE_I2C)
    checkPWMInput();

  // 3. Update LEDs / WiFi
  if ((currMillis - lastMillis) >= (1000 / MIN_FPS))
  {
    if (wifi)
    {
      handleWebSocket();
    }
    else
    {
      updateLEDs();
      FastLED.show();
    }

    fps = 1000.0 / (currMillis - lastMillis);
    lastMillis = currMillis;
  }
  yield();
}

// ---------------- AUTO-SWITCH LOGIC ----------------
void checkInterfaceAutoSwitch()
{
  // A. Check for PWM Activity (Interrupt sets this flag)
  if (pwmActiveDetected && currentMode != MODE_PWM)
  {
    Serial.println("Auto-Switch: PWM Detected");
    switchMode(MODE_PWM);
    pwmActiveDetected = false;
  }

  // B. Check for Digital Activity
  if (currentMode == MODE_I2C)
  {
    if (digitalRead(PIN_SCL_CLOCK) == LOW)
    {
      if (sclLowStartTime == 0)
        sclLowStartTime = millis();

      if (millis() - sclLowStartTime > 50)
      {
        Serial.println("Auto-Switch: Digital Detected (SCL driven LOW)");
        switchMode(MODE_DIGITAL);
        sclLowStartTime = 0;
      }
    }
    else
    {
      sclLowStartTime = 0;
    }
  }
}

// ---------------- MODE SWITCHING HELPER ----------------
void switchMode(InputMode newMode)
{
  if (currentMode == newMode)
    return;

  Serial.print("Switching Mode to: ");
  Serial.println(newMode);

  // Cleanup Old Mode
  if (currentMode == MODE_DIGITAL)
  {
    detachInterrupt(digitalPinToInterrupt(PIN_SCL_CLOCK));
  }

  currentMode = newMode;

  // Setup New Mode
  if (newMode == MODE_I2C)
  {
    pinMode(PIN_SCL_CLOCK, INPUT);
    pinMode(PIN_SDA_DATA, INPUT);
    Wire.begin(I2C_ADDR);
    Wire.onReceive(i2cReceiveEvent);
  }
  else if (newMode == MODE_DIGITAL)
  {
    pinMode(PIN_SCL_CLOCK, INPUT_PULLUP);
    pinMode(PIN_SDA_DATA, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SCL_CLOCK), onDigitalClockRise, RISING);
  }
  else if (newMode == MODE_PWM)
  {
    // Ensure PWM pin is input
    pinMode(PIN_PWM, INPUT);
    // Re-attach interrupt just in case
    attachInterrupt(digitalPinToInterrupt(PIN_PWM), onPWMChange, CHANGE);
  }
}

// --- DIGITAL (Clock + Data) ---
IRAM_ATTR void onDigitalClockRise()
{
  int val = digitalRead(PIN_SDA_DATA);
  if (val)
    digCurrByte |= (1 << (7 - digBitIdx));
  else
    digCurrByte &= ~(1 << (7 - digBitIdx));

  digBitIdx++;

  if (digBitIdx >= 8)
  {
    digBitIdx = 0;
    if (digByteIdx == 0)
    {
      if (digCurrByte == 0xAA)
        digByteIdx = 1;
    }
    else if (digByteIdx < 64)
    {
      digBuffer[digByteIdx - 1] = digCurrByte;
      digByteIdx++;
    }
    digCurrByte = 0;
  }
  lastDigTime = millis();
}

void checkDigitalInput()
{
  if (digByteIdx > 1 && (millis() - lastDigTime > 5))
  {
    int packetLen = digBuffer[0];
    if (digByteIdx - 1 >= packetLen)
    {
      processPacket((uint8_t *)&digBuffer[1], packetLen);
    }
    digByteIdx = 0;
    digBitIdx = 0;
    digCurrByte = 0;
  }
}

// --- PWM (Pulse Width) ---
// This interrupt only detects activity to trigger Auto-Switch.
// The actual reading happens in loop() via pulseIn() for accuracy.
IRAM_ATTR void onPWMChange()
{
  pwmActiveDetected = true;
}

void checkPWMInput()
{
  // 1. Read the Pulse
  // Timeout 40ms (Wait slightly less than the robot's bit delay)
  unsigned long pulse = pulseIn(PIN_PWM, HIGH, 40000);

  // 2. Validate Pulse
  if (pulse > MIN_PULSE && pulse < MAX_PULSE)
  {

    // 3. Decode the Nibble (0-15) based on 500-2500 range
    int nibble = (int)((pulse - (BASE_OFFSET - 66)) / STEP_SIZE);
    if (nibble < 0)
      nibble = 0;
    if (nibble > 15)
      nibble = 15;

    // 4. Process the Nibble
    handleNibble(nibble);

    // 5. CRITICAL TIMING SYNC
    // The Robot holds the signal for 60ms. We wait 30ms to ensure we
    // skip duplicate reads and catch the next fresh bit.
    delay(30);
  }
}

void handleNibble(int val)
{
  // --- DETECT HEADER (0xA, 0xA) ---
  static int lastVal = -1;

  if (lastVal == 0xA && val == 0xA)
  {
    receiving = true;
    pwmIdx = 0;
    highNibbleReceived = false;
    bytesExpected = 0;
    // Serial.println("Header Detected!");
    lastVal = -1;
    return;
  }
  lastVal = val;

  if (!receiving)
    return;

  // --- ASSEMBLE BYTES ---
  if (!highNibbleReceived)
  {
    highNibble = val;
    highNibbleReceived = true;
  }
  else
  {
    // Combine High and Low nibbles
    byte combinedByte = (highNibble << 4) | (val & 0x0F);
    highNibbleReceived = false;

    // Store in Buffer
    pwmBuffer[pwmIdx++] = combinedByte;

    // First byte is always Length
    if (pwmIdx == 1)
    {
      bytesExpected = combinedByte;
    }

    // Check if packet is complete
    if (pwmIdx > 0 && pwmIdx == (bytesExpected + 1))
    {
      // Data starts at index 1 (skip length byte)
      processPacket((uint8_t *)&pwmBuffer[1], bytesExpected);
      receiving = false;
    }
  }
}

// ---------------- I2C HANDLER ----------------
void i2cReceiveEvent(int bytesReceived)
{
  if (currentMode != MODE_I2C)
    return;
  if (bytesReceived > 32)
    bytesReceived = 32;

  uint8_t data[32];
  int i = 0;
  while (Wire.available() && i < bytesReceived)
  {
    data[i++] = Wire.read();
  }
  processPacket(data, bytesReceived);
}

// -- -- -- -- -- -- -- --PACKET PROCESSOR-- -- -- -- -- -- -- --
void processPacket(uint8_t *data, int length)
{
  if (length == 0)
    return;

  // --- DEBUG START ---
  Serial.print("PKT [");
  Serial.print(length);
  Serial.print("]: ");
  for (int i = 0; i < length; i++)
  {
    Serial.print(data[i]);
    Serial.print(" ");
  }
  Serial.println();
  // --- DEBUG END ---

  if (length <= 2)
    handleSettingsPacket(data);
  else if (length <= 5)
    handleStartPacket(data);
  else if (length <= 25)
    handleStripPacket(data);
  else if (length <= 32)
    handleWiFiPacket(data);
}

// ---------------- PACKET PARSERS ----------------
void handleSettingsPacket(uint8_t *data)
{
  Serial.println(" -> TYPE: SETTINGS"); // DEBUG
  quality = data[0];
  maxCurrent = data[1] / 10.0;
  FastLED.setMaxPowerInVoltsAndMilliamps(5, maxCurrent * 1000);
}

void handleStartPacket(uint8_t *data)
{
  Serial.println(" -> TYPE: START"); // DEBUG
  int chIndex = data[0];
  if (chIndex >= MAX_CHANNELS)
    return;

  // Note: channels are statically allocated, just update count
  if (chIndex + 1 > numChannels)
    numChannels = chIndex + 1;

  LEDChannel &ch = channels[chIndex];
  ch.numLEDs = ((int)data[2] << 8) | data[3];
  if (ch.numLEDs > LEDS_PER_CHANNEL)
    ch.numLEDs = LEDS_PER_CHANNEL;

  static bool ledsAdded[MAX_CHANNELS] = {false};

  if (!ledsAdded[chIndex])
  {
    Serial.print("    -> Initializing Channel: ");
    Serial.println(chIndex); // DEBUG
    switch (chIndex)
    {
    case 0:
      FastLED.addLeds<WS2812, 13, GRB>(ch.leds, LEDS_PER_CHANNEL);
      break;
    case 1:
      FastLED.addLeds<WS2812, 12, GRB>(ch.leds, LEDS_PER_CHANNEL);
      break;
    case 2:
      FastLED.addLeds<WS2812, 14, GRB>(ch.leds, LEDS_PER_CHANNEL);
      break;
    case 3:
      FastLED.addLeds<WS2812, 2, GRB>(ch.leds, LEDS_PER_CHANNEL);
      break;
    case 4:
      FastLED.addLeds<WS2812, 16, GRB>(ch.leds, LEDS_PER_CHANNEL);
      break;
    }
    ledsAdded[chIndex] = true;
  }
}

void handleStripPacket(uint8_t *data)
{
  Serial.println(" -> TYPE: STRIP DATA"); // DEBUG
  int chIndex = data[0];
  if (chIndex < 0 || chIndex >= numChannels)
  {
    Serial.print("    -> ERR: Invalid Channel ");
    Serial.println(chIndex); // DEBUG
    return;
  }

  LEDChannel &ch = channels[chIndex];
  ch.h = ((int)data[1] << 8) | data[2];
  ch.s = data[3];
  ch.brightness = data[4];
  ch.rainbow = data[5] != 0;
  ch.colorPeriod = ((int)data[6] << 8) | data[7];
  ch.hueMin = ((int)data[8] << 8) | data[9];
  ch.hueMax = ((int)data[10] << 8) | data[11];
  ch.pattern = data[12];
  ch.trailLength = data[13];
  ch.sourceCenter = ((int)data[14] << 8) | data[15];
  ch.sourceLength = data[16];
  ch.gradient = data[17] != 0;
  ch.moving = data[18] != 0;
  ch.period = ((int)data[19] << 8) | data[20];
  ch.direction = data[21] != 0;

  Serial.print("    -> Pattern: ");
  Serial.print(ch.pattern);
  Serial.print(" Hue: ");
  Serial.println(ch.h);
}

void handleWiFiPacket(uint8_t *data)
{
  Serial.println(" -> TYPE: WIFI"); // DEBUG
  if (!wifi)
  {
    wifi = true;
    FastLED.clear();
    FastLED.show();
    char rxBuffer[33];
    memcpy(rxBuffer, data, 32);
    rxBuffer[32] = '\0';
    trimSSID(rxBuffer);
    startWebSocket(rxBuffer);
  }
}

void trimSSID(char *str)
{
  int len = strlen(str);
  while (len > 0 && str[len - 1] == ' ')
  {
    str[len - 1] = '\0';
    len--;
  }
}

CRGB normalizeLEDs(CRGB c) { return c; }

// ---------------- UPDATE PATTERNS ----------------
void updateLEDs()
{
  unsigned long now = millis();
  for (int ch = 0; ch < numChannels; ch++)
  {
    LEDChannel &c = channels[ch];
    applyPattern(c);
    c.lastUpdate = now;
  }
}

void applyPattern(LEDChannel &ch)
{
  unsigned long now = millis();
  CRGB baseColor;
  if (ch.rainbow)
  {
    uint8_t hue = (now * 255 / ch.colorPeriod) % 256;
    baseColor = CHSV(hue, 255, ch.brightness);
  }
  else
  {
    baseColor = CHSV(ch.h, ch.s, ch.brightness);
  }

  switch (ch.pattern)
  {
  case 1: // SOLID
    if (ch.gradient)
      fill_gradient(ch.leds, ch.numLEDs, CHSV(ch.hueMin, 255, ch.brightness), CHSV(ch.hueMax, 255, ch.brightness));
    else
      fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(baseColor));
    break;
  case 2: // BLINK
    if ((now / ch.period) % 2 == 0)
      fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(baseColor));
    else
      fill_solid(ch.leds, ch.numLEDs, CRGB::Black);
    break;
  case 3: // FADE
  {
    float progress = (float)(now % ch.period) / ch.period;
    uint8_t fadeBr = ch.brightness * (1.0f - progress);
    fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(CHSV(ch.h, ch.s, fadeBr)));
  }
  break;
  case 4: // BREATHE
  {
    float intensity = (sin(now * (2.0 * PI / ch.period)) + 1.0) / 2.0;
    fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(CHSV(ch.h, ch.s, intensity * ch.brightness)));
  }
  break;
  case 5: // LARSON SCANNER
    fadeToBlackBy(ch.leds, ch.numLEDs, 64);
    ch.pos[0] = (now * ch.numLEDs / ch.period) % ch.numLEDs;
    ch.leds[ch.pos[0]] = normalizeLEDs(baseColor);
    break;
  case 6: // PINGPONG
  {
    fadeToBlackBy(ch.leds, ch.numLEDs, ch.trailLength);
    int totalSteps = (ch.numLEDs - 1) * 2;
    int step = (now * totalSteps / ch.period) % totalSteps;
    ch.pos[0] = (step < ch.numLEDs) ? step : totalSteps - step;
    ch.leds[ch.pos[0]] = normalizeLEDs(baseColor);
  }
  break;
  case 7: // RADAR
  {
    fadeToBlackBy(ch.leds, ch.numLEDs, ch.trailLength);
    int maxDist = max(ch.sourceCenter, (int)ch.numLEDs - ch.sourceCenter);
    int dist = (now * maxDist / ch.period) % maxDist;
    if (!ch.direction)
      dist = maxDist - dist;
    if (ch.sourceCenter + dist < ch.numLEDs)
      ch.leds[ch.sourceCenter + dist] = normalizeLEDs(baseColor);
    if (ch.sourceCenter - dist >= 0)
      ch.leds[ch.sourceCenter - dist] = normalizeLEDs(baseColor);
  }
  break;
  case 8: // SCANNER
  {
    int growth = (now * ch.numLEDs / ch.period) % ch.numLEDs;
    fill_solid(ch.leds, ch.numLEDs, CRGB::Black);
    if (ch.direction)
      fill_solid(ch.leds, growth, normalizeLEDs(baseColor));
    else
      fill_solid(&ch.leds[ch.numLEDs - growth], growth, normalizeLEDs(baseColor));
  }
  break;
  case 9: // WAVE
    for (int i = 0; i < ch.numLEDs; i++)
    {
      uint8_t wave = beatsin8(ch.period / 100, 0, ch.brightness, 0, i * 10);
      ch.leds[i] = normalizeLEDs(CHSV(ch.h, ch.s, wave));
    }
    break;
  case 10: // SPARKLE
    fadeToBlackBy(ch.leds, ch.numLEDs, 10);
    if (random8() < 30)
      ch.leds[random16(ch.numLEDs)] = normalizeLEDs(baseColor);
    break;
  case 11: // HEARTBEAT
  {
    uint8_t br = beatsin8(60000 / ch.period, 0, ch.brightness, 0, 0);
    if (br < 10)
      br = beatsin8(60000 / ch.period, 0, ch.brightness / 2, 0, 160);
    fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(CHSV(ch.h, ch.s, br)));
  }
  break;
  case 12: // CANDY CANE
    for (int i = 0; i < ch.numLEDs; i++)
    {
      bool isAlt = (i / ch.trailLength) % 2 == 0;
      ch.leds[i] = isAlt ? normalizeLEDs(baseColor) : normalizeLEDs(CRGB::White);
    }
    break;
  case 13: // MOVING CANDY CANE
  {
    int offset = (now * (ch.trailLength * 2) / ch.period) % (ch.trailLength * 2);
    for (int i = 0; i < ch.numLEDs; i++)
    {
      bool isAlt = ((i + offset) / ch.trailLength) % 2 == 0;
      ch.leds[i] = isAlt ? normalizeLEDs(baseColor) : normalizeLEDs(CRGB::White);
    }
  }
  break;
  case 14: // FIREWORK
  {
    int maxDist = max(ch.sourceCenter, (int)ch.numLEDs - ch.sourceCenter);
    float progress = (float)(now % ch.period) / ch.period;
    int dist = maxDist * (1.0f - progress);
    uint8_t br = ch.brightness * (1.0f - progress);
    fill_solid(ch.leds, ch.numLEDs, CRGB::Black);
    if (ch.sourceCenter + dist < ch.numLEDs)
      ch.leds[ch.sourceCenter + dist] = normalizeLEDs(CHSV(ch.h, ch.s, br));
    if (ch.sourceCenter - dist >= 0)
      ch.leds[ch.sourceCenter - dist] = normalizeLEDs(CHSV(ch.h, ch.s, br));
  }
  break;
  default:
    fill_solid(ch.leds, ch.numLEDs, normalizeLEDs(baseColor));
    break;
  }
}

// ---------------- WEB / WIFI ----------------
void startWebSocket(const char *ssid)
{
  if (!LittleFS.begin())
    Serial.println("LittleFS Error");
  else
    Serial.println("LittleFS mounted");

  WiFi.softAP(ssid);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(myIP);

  server.onNotFound([]()
                    {
    if (!handleFileRead(server.uri())) server.send(404, "text/plain", "FileNotFound"); });

  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket started.");
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    clientConnected = false;
    break;
  case WStype_CONNECTED:
  {
    clientConnected = true;
    handleWebSocket();
    break;
  }
  }
}

void handleWebSocket()
{
  if (!wifi || numChannels == 0)
    return;
  server.handleClient();
  webSocket.loop();

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < numChannels; i++)
  {
    JsonObject obj = array.add<JsonObject>();
    obj["numLEDs"] = channels[i].numLEDs;
    obj["h"] = channels[i].h;
    obj["s"] = channels[i].s;
    obj["brightness"] = channels[i].brightness;
    obj["pattern"] = channels[i].pattern;
  }

  String data;
  serializeJson(doc, data);
  webSocket.broadcastTXT(data);
}

bool handleFileRead(String path)
{
  if (path.endsWith("/"))
    path += "index.html";
  if (LittleFS.exists(path))
  {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, "text/html");
    file.close();
    return true;
  }
  return false;
}