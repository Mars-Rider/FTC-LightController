#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>
#include <FastLED.h>
#include <LittleFS.h>

// --- Configuration ---
const char *ap_ssid = "Cannon Lite";
const char *ap_password = "12345678";

#define NUM_LEDS 130
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];
ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// --- State Variables ---
uint8_t brightness = 150;
uint8_t speed = 30;
uint8_t currentPattern = 0;
uint32_t selectedColorHex = 0x00D4FF;
CRGB selectedColor = 0x00D4FF;
uint8_t gHue = 0;
unsigned long lastSave = 0;
bool needsSave = false;

// --- HTML Dashboard (Saved in PROGMEM) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Cannon Lite Kinetic</title>
    <style>
        body { font-family: sans-serif; background: #0a0a0a; color: white; text-align: center; margin: 0; padding: 20px; }
        .card { background: #1a1a1a; padding: 25px; border-radius: 20px; max-width: 400px; margin: auto; }
        select, input, button { width: 100%; margin: 10px 0; padding: 15px; border-radius: 10px; border: none; background: #2a2a2a; color: white; font-size: 16px; }
        .label { text-align: left; font-size: 12px; color: #888; margin-top: 10px; text-transform: uppercase; }
    </style>
</head>
<body>
    <div class="card">
        <h1>CANNON LITE</h1>
        <div class="label">Color</div>
        <input type="color" id="colorWheel" onchange="sendColor(this.value)">
        <div class="label">Pattern</div>
        <select id="patternSelect" onchange="sendPattern(this.value)">
            <option value="0">Solid Kinetic</option>
            <option value="1">Kinetic Pulse</option>
            <option value="2">Ocean Wave</option>
            <option value="3">Fluid Larson</option>
            <option value="4">Plasma Rainbow</option>
            <option value="5">Stardust Sparks</option>
            <option value="6">Kinetic Popcorn</option>
            <option value="7">Neon Red/White</option>
            <option value="8">Glitter Flow</option>
        </select>
        <div class="label">Brightness</div>
        <input type="range" min="0" max="255" onchange="sendVal('B', this.value)">
        <div class="label">Speed</div>
        <input type="range" min="10" max="150" onchange="sendVal('S', this.value)">
    </div>
    <script>
        var gateway = `ws://${window.location.hostname}:81/`;
        var websocket;
        function init() { websocket = new WebSocket(gateway); }
        function sendColor(h) { websocket.send(`C ${h.replace('#','')}`); }
        function sendPattern(v) { websocket.send(`P ${v}`); }
        function sendVal(t, v) { websocket.send(`${t} ${v}`); }
        window.onload = init;
    </script>
</body>
</html>
)rawliteral";

// --- Persistence (Save/Load) ---
void saveConfig()
{
  File f = LittleFS.open("/config.bin", "w");
  if (f)
  {
    f.write((uint8_t *)&brightness, 1);
    f.write((uint8_t *)&speed, 1);
    f.write((uint8_t *)&currentPattern, 1);
    f.write((uint8_t *)&selectedColorHex, 4);
    f.close();
    Serial.println("Config Saved to LittleFS");
  }
}

void loadConfig()
{
  if (LittleFS.exists("/config.bin"))
  {
    File f = LittleFS.open("/config.bin", "r");
    if (f)
    {
      f.read((uint8_t *)&brightness, 1);
      f.read((uint8_t *)&speed, 1);
      f.read((uint8_t *)&currentPattern, 1);
      f.read((uint8_t *)&selectedColorHex, 4);
      selectedColor = selectedColorHex;
      f.close();
      Serial.println("Config Loaded");
    }
  }
}

// --- Animation Functions ---
void kineticPulse()
{
  uint8_t val = beatsin8(speed, 0, 255);
  fill_solid(leds, NUM_LEDS, selectedColor);
  FastLED.setBrightness(map(val, 0, 255, 0, brightness));
}

void plasmaRainbow()
{
  for (int i = 0; i < NUM_LEDS; i++)
  {
    uint8_t colorIndex = cubicwave8(i * 4 + gHue);
    leds[i] = CHSV(colorIndex + gHue, 255, 255);
  }
}

void runPattern()
{
  FastLED.setBrightness(brightness);
  switch (currentPattern)
  {
  case 0:
    fill_solid(leds, NUM_LEDS, selectedColor);
    break;
  case 1:
    kineticPulse();
    break;
  case 4:
    plasmaRainbow();
    break;
  // ... (Add other patterns from previous response here)
  default:
    fill_solid(leds, NUM_LEDS, selectedColor);
    break;
  }
  EVERY_N_MILLISECONDS(20) { gHue++; }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  if (type == WStype_TEXT)
  {
    String msg = (char *)payload;
    if (msg.startsWith("C"))
    {
      selectedColorHex = strtol(msg.substring(2).c_str(), NULL, 16);
      selectedColor = selectedColorHex;
    }
    else if (msg.startsWith("P"))
    {
      currentPattern = msg.substring(2).toInt();
    }
    else if (msg.startsWith("B"))
    {
      brightness = msg.substring(2).toInt();
    }
    else if (msg.startsWith("S"))
    {
      speed = msg.substring(2).toInt();
    }
    needsSave = true;
    lastSave = millis();
  }
}

void setup()
{
  Serial.begin(9600);
  LittleFS.begin();
  loadConfig();

  // Init All Pins
  FastLED.addLeds<LED_TYPE, 13, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, 12, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, 14, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, 2, COLOR_ORDER>(leds, NUM_LEDS);

  FastLED.setBrightness(brightness);

  Serial.println("Setting Up");

  WiFi.softAP(ap_ssid, ap_password);
  MDNS.begin("CannonLite");
  server.on("/", []()
            { server.send(200, "text/html", index_html); });
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop()
{
  webSocket.loop();
  server.handleClient();
  runPattern();
  FastLED.show();

  // Non-blocking yield to prevent freezing
  yield();

  // Auto-save after 5 seconds of no changes
  if (needsSave && millis() - lastSave > 5000)
  {
    saveConfig();
    needsSave = false;
  }
}