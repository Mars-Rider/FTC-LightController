#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>
#include <FastLED.h>
#include <LittleFS.h>

// --- HARDWARE LIMITS ---
#define MAX_LEDS 600       
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// --- Globals ---
CRGB leds[MAX_LEDS];
ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// --- Configurable Variables (RAM) ---
// Defaults (used if no config file exists)
uint8_t brightness = 150;
uint8_t speed = 30;
uint8_t currentPattern = 0;
uint32_t selectedColorHex = 0xFF0000;
CRGB selectedColor = CRGB::Red;

uint16_t ledCount = 130;    
uint8_t selectedPinIdx = 0; // 0=Pin1(D7)

char ap_ssid[33] = "Cannon Lite";     // Max 32 chars + null
char ap_pass[65] = "12345678";        // Max 64 chars + null

// Animation Helpers
uint8_t gHue = 0;
bool needsSave = false;
unsigned long lastSaveTime = 0;

// -------------------------------------------------------------------------
// 1. FILE SYSTEM (LittleFS) & CONFIG STRUCTURE
// -------------------------------------------------------------------------
struct ConfigData {
    uint8_t bri;
    uint8_t spd;
    uint8_t pat;
    uint32_t col;
    uint16_t cnt;
    uint8_t pin;
    char ssid[33];
    char pass[65];
};

void saveConfig() {
    File f = LittleFS.open("/config.bin", "w");
    if (!f) return;
    
    ConfigData cfg;
    cfg.bri = brightness;
    cfg.spd = speed;
    cfg.pat = currentPattern;
    cfg.col = selectedColorHex;
    cfg.cnt = ledCount;
    cfg.pin = selectedPinIdx;
    
    // Copy strings safely
    strncpy(cfg.ssid, ap_ssid, 32); cfg.ssid[32] = 0;
    strncpy(cfg.pass, ap_pass, 64); cfg.pass[64] = 0;

    f.write((uint8_t*)&cfg, sizeof(ConfigData));
    f.close();
    Serial.println("Config Saved");
}

void loadConfig()
{
  if (LittleFS.exists("/config.bin"))
  {
    File f = LittleFS.open("/config.bin", "r");

    // SAFETY CHECK: If the file size doesn't match our new structure,
    // it means it's an old config file. Delete it and use defaults.
    if (f.size() != sizeof(ConfigData))
    {
      Serial.println("Config Version Mismatch! Deleting old config...");
      f.close();
      LittleFS.remove("/config.bin");
      return;
    }

    if (f)
    {
      ConfigData cfg;
      f.read((uint8_t *)&cfg, sizeof(ConfigData));
      f.close();

      brightness = cfg.bri;
      speed = cfg.spd;
      currentPattern = cfg.pat;
      selectedColorHex = cfg.col;
      selectedColor = selectedColorHex;

      // Validate Hardware Configs
      ledCount = (cfg.cnt > 0 && cfg.cnt <= MAX_LEDS) ? cfg.cnt : 130;
      selectedPinIdx = (cfg.pin <= 4) ? cfg.pin : 0;

      // Load Network Config - Only if they contain valid text
      if (strlen(cfg.ssid) > 0)
        strncpy(ap_ssid, cfg.ssid, 32);
      if (strlen(cfg.pass) > 0)
        strncpy(ap_pass, cfg.pass, 64);

      Serial.println("Config Loaded");
    }
  }
}

// -------------------------------------------------------------------------
// 2. ANIMATION PATTERNS
// -------------------------------------------------------------------------
void patSolid() { fill_solid(leds, ledCount, selectedColor); }

void patBlink() {
  uint8_t val = beatsin8(map(speed, 1, 255, 10, 120), 0, 255);
  fill_solid(leds, ledCount, selectedColor);
  fadeToBlackBy(leds, ledCount, 255 - val);
}

void patWave() {
  fill_solid(leds, ledCount, CRGB::Black);
  int pos = beatsin16(map(speed, 1, 255, 10, 60), 0, ledCount - 1);
  leds[pos] = selectedColor;
  if (pos > 0) leds[pos - 1] = selectedColor;
  if (pos < ledCount - 1) leds[pos + 1] = selectedColor;
  blur1d(leds, ledCount, 100);
}

void patLarson() {
  fadeToBlackBy(leds, ledCount, 20);
  int pos = beatsin16(map(speed, 1, 255, 10, 60), 0, ledCount - 1);
  leds[pos] += selectedColor;
}

void patRainbow() {
  fill_rainbow(leds, ledCount, gHue, 7);
}

void patFirework() {
  fadeToBlackBy(leds, ledCount, 20);
  if (random8() < map(speed, 0, 255, 2, 20)) {
    leds[random16(ledCount)] = CHSV(random8(), 200, 255);
  }
}

void patPopcorn() {
  fadeToBlackBy(leds, ledCount, 40);
  if (random8() < map(speed, 0, 255, 5, 50)) {
    leds[random16(ledCount)] = selectedColor;
  }
}

void patRW() {
  uint8_t mSpeed = map(speed, 1, 255, 50, 2);
  for (int i = 0; i < ledCount; i++) {
    uint8_t v = sin8(i * 5 + millis() / mSpeed);
    leds[i] = (v > 128) ? CRGB::Red : CRGB::Grey;
  }
}

void patExtended(uint8_t mode) {
  uint8_t eff = mode - 8;
  CRGBPalette16 pal;
  if (eff < 5) pal = OceanColors_p;
  else if (eff < 10) pal = ForestColors_p;
  else if (eff < 15) pal = LavaColors_p;
  else if (eff < 20) pal = PartyColors_p;
  else if (eff < 25) pal = CloudColors_p;
  else pal = RainbowColors_p;

  uint8_t zoom = 10 + (eff % 5) * 5;
  uint8_t moveSpeed = map(speed, 1, 255, 60, 1);

  for (int i = 0; i < ledCount; i++) {
    uint8_t colorIndex = inoise8(i * zoom, millis() / moveSpeed);
    leds[i] = ColorFromPalette(pal, colorIndex, 255, LINEARBLEND);
  }
}

void runPattern() {
  FastLED.setBrightness(brightness);
  switch (currentPattern) {
    case 0: patSolid(); break;
    case 1: patBlink(); break;
    case 2: patWave(); break;
    case 3: patLarson(); break;
    case 4: patRainbow(); break;
    case 5: patFirework(); break;
    case 6: patPopcorn(); break;
    case 7: patRW(); break;
    default: if (currentPattern >= 8) patExtended(currentPattern); break;
  }
  
  static unsigned long lastHueTime = 0;
  int hueDelay = map(speed, 1, 255, 60, 5); 
  if (millis() - lastHueTime >= (unsigned long)hueDelay) {
      gHue++;
      lastHueTime = millis();
  }
}

// -------------------------------------------------------------------------
// 3. WEB PAGE (PROGMEM)
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// 3. WEB PAGE (PROGMEM)
// -------------------------------------------------------------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Cannon Lite</title>
<style>
  body { font-family: 'Segoe UI', sans-serif; background: #121212; color: #eee; text-align: center; padding: 20px; margin: 0; }
  
  /* Container Box */
  .box { background: #1e1e1e; padding: 20px; border-radius: 12px; margin-bottom: 20px; border: 1px solid #333; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
  
  /* Labels */
  label { font-size: 11px; color: #888; text-transform: uppercase; letter-spacing: 1px; display: block; text-align: left; margin-top: 15px; margin-bottom: 5px; }
  
  /* --- CUSTOM INPUT STYLES --- */
  
  /* Text Inputs & Selects */
  select, input[type="text"], input[type="number"] { 
      width: 100%; padding: 12px; border-radius: 8px; border: 1px solid #333; 
      font-size: 16px; background: #2c2c2c; color: white; outline: none; box-sizing: border-box;
  }
  select:focus, input:focus { border-color: #2196F3; } /* Focus Blue */

  /* Color Picker */
  input[type="color"] {
      -webkit-appearance: none; border: none; width: 100%; height: 50px; padding: 0; 
      background: none; border-radius: 8px; cursor: pointer; overflow: hidden;
  }
  input[type="color"]::-webkit-color-swatch-wrapper { padding: 0; }
  input[type="color"]::-webkit-color-swatch { border: none; border-radius: 8px; }

  /* Range Sliders */
  input[type=range] {
      -webkit-appearance: none; width: 100%; background: transparent; margin: 10px 0;
  }
  input[type=range]:focus { outline: none; }
  
  /* Slider Track (The darker blue background) */
  input[type=range]::-webkit-slider-runnable-track {
      width: 100%; height: 8px; cursor: pointer; background: #0D47A1; border-radius: 4px;
  }
  /* Slider Thumb (The bright blue handle) */
  input[type=range]::-webkit-slider-thumb {
      height: 20px; width: 20px; border-radius: 50%; background: #2196F3; 
      cursor: pointer; -webkit-appearance: none; margin-top: -6px; /* center thumb */
      box-shadow: 0 0 5px rgba(0,0,0,0.5);
  }

  /* Buttons */
  button { 
      width: 100%; padding: 14px; margin-top: 10px; border-radius: 8px; border: none; 
      font-size: 14px; font-weight: bold; cursor: pointer; transition: 0.2s; 
      text-transform: uppercase; letter-spacing: 0.5px;
  }
  button:active { transform: scale(0.98); opacity: 0.9; }
  
  .refresh-btn { background: #c62828; color: white; margin-top: 20px; }
  .settings-btn { background: #333; color: #aaa; margin-top: 10px; font-size: 12px; }
  .reboot-btn { background: #ff9800; color: black; margin-top: 20px; }

  /* Status Indicators */
  .status-dot { height: 8px; width: 8px; background-color: #f44336; border-radius: 50%; display: inline-block; margin-right: 5px; }
  .online { background-color: #4caf50; box-shadow: 0 0 8px #4caf50; }
  .warning { color: #ff9800; font-size: 11px; font-weight: bold; text-align: left; margin-bottom: 15px; border-bottom: 1px solid #333; padding-bottom: 10px; display:block; }
  
  #settingsMenu { display: none; }
</style>
</head>
<body>
  <h2 style="letter-spacing: 2px; margin-bottom: 5px;">CANNON LITE</h2>
  
  <div id="mainControls">
    <div class="box">
        <label>Color Selection</label>
        <input type="color" id="clr" oninput="sendV('C', this.value)">
        
        <label>Pattern Mode</label>
        <select id="pat" onchange="sendV('P', this.value)">
        <optgroup label="Core Patterns">
            <option value="0">Solid Color</option>
            <option value="1">Blink</option>
            <option value="2">Wave</option>
            <option value="3">Larson Scanner</option>
            <option value="4">Rainbow Cycle</option>
            <option value="5">Firework</option>
            <option value="6">Popcorn</option>
            <option value="7">Red/White Neon</option>
        </optgroup>
        <optgroup label="Extended Library" id="ext"></optgroup>
        </select>

        <label>Brightness</label>
        <input type="range" min="0" max="255" id="bri" oninput="sendV('B', this.value)">
        
        <label>Speed / Intensity</label>
        <input type="range" min="1" max="255" id="spd" oninput="sendV('S', this.value)">
    </div>

    <div class="box">
        <div style="font-size: 12px; color: #888; margin-bottom: 10px;">
            <span id="dot" class="status-dot"></span> <span id="stat">Disconnected</span>
        </div>
        <button class="settings-btn" onclick="toggleSettings()">SETTINGS & SETUP</button>
        <button class="refresh-btn" onclick="forceRefresh()">REFRESH CONNECTION</button>
    </div>
  </div>

  <div id="settingsMenu">
      <div class="box" style="border-color: #ff9800;">
        <span class="warning">⚠️ SYSTEM CONFIGURATION</span>
        
        <label>LED Strip Length (Max 600)</label>
        <input type="number" id="ledCnt" min="1" max="600" onchange="sendV('L', this.value)">

        <label>Output Pin</label>
        <select id="pinSel" onchange="sendV('X', this.value)">
            <option value="0">Pin 1 (GPIO 13)</option>
            <option value="1">Pin 2 (GPIO 12)</option>
            <option value="2">Pin 3 (GPIO 14)</option>
            <option value="3">Pin 4 (GPIO 2)</option>
            <option value="4">Pin 5 (GPIO 16)</option>
        </select>

        <label style="margin-top:20px; color:#ff9800">WiFi Settings</label>
        <label>Network Name (SSID)</label>
        <input type="text" id="ssid" onchange="sendStr('U', this.value)">
        
        <label>Password (Min 8 Chars)</label>
        <input type="text" id="pass" onchange="sendStr('W', this.value)">

        <button class="reboot-btn" onclick="doReboot()">SAVE & REBOOT</button>
        <button class="settings-btn" onclick="toggleSettings()">BACK TO CONTROLS</button>
      </div>
  </div>

<script>
  var ws;
  function init() {
    ws = new WebSocket('ws://' + window.location.hostname + ':81/');
    ws.onopen = () => {
        document.getElementById('stat').innerText = 'Online';
        document.getElementById('dot').classList.add('online');
    };
    ws.onclose = () => {
        document.getElementById('stat').innerText = 'Offline';
        document.getElementById('dot').classList.remove('online');
    };
    ws.onmessage = (event) => {
        var msg = event.data;
        if(msg.startsWith("{")) {
            var j = JSON.parse(msg);
            document.getElementById('ssid').value = j.ssid;
            document.getElementById('pass').value = j.pass;
        } else {
            var d = msg.split(','); 
            if(d.length >= 6) {
                document.getElementById('pat').value = d[0];
                document.getElementById('bri').value = d[1];
                document.getElementById('spd').value = d[2];
                document.getElementById('clr').value = '#' + parseInt(d[3]).toString(16).padStart(6, '0');
                document.getElementById('pinSel').value = d[4];
                document.getElementById('ledCnt').value = d[5];
            }
        }
    };

    if(document.getElementById('ext').children.length === 0) {
        var ext = document.getElementById('ext');
        for(var i=8; i<45; i++) {
            var opt = document.createElement('option');
            opt.value = i;
            opt.innerHTML = "Effect Variation " + (i-7);
            ext.appendChild(opt);
        }
    }
  }
  
  function forceRefresh() { if(ws) ws.close(); init(); }

  function sendV(t, v) { 
      if(t==='C') v = v.replace('#','');
      ws.send(t + v); 
  }

  function sendStr(t, v) { ws.send(t + v); }

  function doReboot() {
      if(confirm("Device will reboot to apply settings. Reconnect to new WiFi if changed.")) {
          ws.send("REBOOT");
      }
  }

  function toggleSettings() {
      var m = document.getElementById('mainControls');
      var s = document.getElementById('settingsMenu');
      if(m.style.display === 'none') {
          m.style.display = 'block';
          s.style.display = 'none';
      } else {
          m.style.display = 'none';
          s.style.display = 'block';
      }
  }
  
  window.onload = init;
</script>
</body>
</html>
)rawliteral";

// -------------------------------------------------------------------------
// 4. WEBSOCKET LOGIC
// -------------------------------------------------------------------------
void sendCurrentState() {
  // 1. Send Control CSV
  String msg = String(currentPattern) + "," +
               String(brightness) + "," +
               String(speed) + "," +
               String(selectedColorHex) + "," +
               String(selectedPinIdx) + "," +
               String(ledCount);
  webSocket.broadcastTXT(msg);
  
  // 2. Send Config JSON (for strings)
  String j = "{\"ssid\":\"" + String(ap_ssid) + "\", \"pass\":\"" + String(ap_pass) + "\"}";
  webSocket.broadcastTXT(j);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = (char *)payload;
    
    // Check for Reboot Command
    if(msg == "REBOOT") {
        saveConfig();
        delay(500);
        ESP.restart();
        return;
    }

    char cmd = msg.charAt(0);
    String val = msg.substring(1);

    if (cmd == 'C') {
      selectedColorHex = strtol(val.c_str(), NULL, 16);
      selectedColor = selectedColorHex;
    }
    else if (cmd == 'P') currentPattern = val.toInt();
    else if (cmd == 'B') brightness = val.toInt();
    else if (cmd == 'S') speed = val.toInt();
    else if (cmd == 'X') selectedPinIdx = val.toInt(); 
    else if (cmd == 'L') { 
        int c = val.toInt();
        if(c > MAX_LEDS) c = MAX_LEDS;
        if(c < 1) c = 1;
        ledCount = c;
    }
    else if (cmd == 'U') { // SSID Update
        val.toCharArray(ap_ssid, 32);
    }
    else if (cmd == 'W') { // Pass Update
        val.toCharArray(ap_pass, 64);
    }

    needsSave = true;
    lastSaveTime = millis();
  }
  else if (type == WStype_CONNECTED) {
    sendCurrentState();
  }
}

// -------------------------------------------------------------------------
// 5. SETUP & LOOP
// -------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  LittleFS.begin();

  // 1. Load defaults or saved file
  loadConfig();

  // 2. Init Selected Pin
  switch (selectedPinIdx)
  {
  case 0:
    FastLED.addLeds<LED_TYPE, 13, COLOR_ORDER>(leds, ledCount);
    break; // Pin 1 (D7)
  case 1:
    FastLED.addLeds<LED_TYPE, 12, COLOR_ORDER>(leds, ledCount);
    break; // Pin 2 (D6)
  case 2:
    FastLED.addLeds<LED_TYPE, 14, COLOR_ORDER>(leds, ledCount);
    break; // Pin 3 (D5)
  case 3:
    FastLED.addLeds<LED_TYPE, 2, COLOR_ORDER>(leds, ledCount);
    break; // Pin 4 (D4)
  case 4:
    FastLED.addLeds<LED_TYPE, 16, COLOR_ORDER>(leds, ledCount);
    break; // Pin 5 (D0)
  default:
    FastLED.addLeds<LED_TYPE, 13, COLOR_ORDER>(leds, ledCount);
    break;
  }

  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.clear();
  FastLED.show();

  // 3. Start WiFi
  WiFi.mode(WIFI_AP); // Force Access Point Mode
  WiFi.softAP(ap_ssid, ap_pass);
  MDNS.begin("CannonLite");

  Serial.print("AP Started: ");
  Serial.println(ap_ssid);

  server.on("/", []()
            { server.send(200, "text/html", index_html); });
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  webSocket.loop();
  server.handleClient();
  runPattern();
  FastLED.show();
  yield();

  if (needsSave && (millis() - lastSaveTime > 3000)) {
    saveConfig();
    needsSave = false;
  }
}

