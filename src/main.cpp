// Wi-Fi LED light: the board opens its own Wi-Fi network with a control page
// (http://192.168.4.1, opens by itself on most phones). Optionally a hand-wave
// sensor toggles the light too.
//
// Settings come from platformio.ini, one [env] per location:
//   LOCATION_TITLE     heading on the control page
//   AP_SSID            Wi-Fi network name
//   AP_PASSWORD_DEFAULT  Wi-Fi password, or put AP_PASSWORD in the file named by
//                      include/secrets_kitchen.h when KITCHEN_SECRETS=1 (kept out of
//                      git), at least 8 characters
//   LED_PIN, NUM_LEDS  WS2812B data pin and LED count
//   MAX_BRIGHTNESS     0-255, brightness of "on" (default 255)
//   FADE_MS            fade time for on/off; 0 = instant
//   LIGHT_ON_AT_BOOT   1 = light comes on at power-up (default), 0 = stays off
//   SENSOR_PIN         push button or hand sensor pin; -1 or unset = none
//                      (push button: GPIO to GND, SENSOR_ACTIVE_HIGH=0, SENSOR_PULL=1)
//   SENSOR_ACTIVE_HIGH 1 = sensor output goes HIGH when it sees a hand
//   SENSOR_LATCHING    0 = sensor gives a pulse per wave (each pulse toggles)
//                      1 = sensor flips its output on each wave (each change toggles)
//   SENSOR_PULL        0 = none (default), 1 = pull-up, 2 = pull-down
//   SENSOR_LABEL       name of the input in the serial log (default "sensor")
//   SENSOR_LOCKOUT_MS  pulse inputs: presses closer together than this count once (default 800)
//   DISABLE_BROWNOUT   1 = turn off the brown-out reset (bathroom board only)
//   COLOR_CONTROL      1 = page has brightness, white warmth and colour controls,
//                      saved across power cuts (default 0: warm white, on/off only)

#include <Arduino.h>
#include <NeoPixelBus.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

// The file name is written out (not taken from a macro) so PlatformIO sees
// the dependency and rebuilds when the password changes.
#if defined(KITCHEN_SECRETS) && __has_include("secrets_kitchen.h")
#include "secrets_kitchen.h"
#endif

#ifndef LOCATION_TITLE
#define LOCATION_TITLE "Light"
#endif
#ifndef AP_SSID
#error "Set AP_SSID in platformio.ini"
#endif
#ifndef AP_PASSWORD
#ifdef AP_PASSWORD_DEFAULT
#define AP_PASSWORD AP_PASSWORD_DEFAULT
#else
#error "No Wi-Fi password: copy include/secrets_kitchen.h.example to include/secrets_kitchen.h and set AP_PASSWORD"
#define AP_PASSWORD "unset-password"  // only to keep the compiler from adding more errors
#endif
#endif
static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "AP_PASSWORD needs at least 8 characters");

#ifndef LED_PIN
#error "Set LED_PIN in platformio.ini"
#endif
#ifndef NUM_LEDS
#error "Set NUM_LEDS in platformio.ini"
#endif
#ifndef MAX_BRIGHTNESS
#define MAX_BRIGHTNESS 255
#endif
#ifndef FADE_MS
#define FADE_MS 0
#endif
#ifndef LIGHT_ON_AT_BOOT
#define LIGHT_ON_AT_BOOT 1
#endif
#ifndef SENSOR_PIN
#define SENSOR_PIN -1
#endif
#ifndef SENSOR_ACTIVE_HIGH
#define SENSOR_ACTIVE_HIGH 1
#endif
#ifndef SENSOR_LATCHING
#define SENSOR_LATCHING 0
#endif
#ifndef SENSOR_LABEL
#define SENSOR_LABEL "sensor"
#endif
#ifndef SENSOR_PULL
#define SENSOR_PULL 0
#endif
#ifndef DISABLE_BROWNOUT
#define DISABLE_BROWNOUT 0
#endif
#ifndef COLOR_CONTROL
#define COLOR_CONTROL 0
#endif

#if COLOR_CONTROL
#include <Preferences.h>
#endif

#if DISABLE_BROWNOUT
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4
#define FADE_STEP_MS 10          // fade update interval
#define SENSOR_DEBOUNCE_MS 40    // a level must hold this long to count
#ifndef SENSOR_LOCKOUT_MS
#define SENSOR_LOCKOUT_MS 800    // pulse inputs: ignore further pulses this long after a toggle
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)
NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod> strip(NUM_LEDS, LED_PIN);
#else
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(NUM_LEDS, LED_PIN);
#endif
AsyncWebServer server(80);
DNSServer dnsServer;

// Web requests run on the network task, the strip is driven from loop():
// requests only set the targets, loop() moves the strip towards them.
volatile uint8_t targetBrightness = 0;
uint8_t shownBrightness = 0;
uint32_t lastFadeStepAt = 0;

// Colour of the light at full brightness, 0xRRGGBB. One 32-bit value so a
// web request can't be read half-written.
const uint32_t DEFAULT_WHITE = 0xFFDF96;  // the original warm white (about 4000 K)
volatile uint32_t targetColor = DEFAULT_WHITE;
uint32_t shownColor = DEFAULT_WHITE;

void addSecurityHeaders(AsyncWebServerResponse *response);

// ------------------------------------------------------------------ light

#if COLOR_CONTROL
#define KELVIN_MIN 2200
#define KELVIN_MAX 6500
#define KELVIN_DEFAULT 4000
#define COLOR_STEP 12            // per FADE_STEP_MS: a full colour change takes about 0.2 s

enum LightMode : uint8_t { MODE_WHITE = 0, MODE_COLOR = 1 };
volatile uint8_t lightMode = MODE_WHITE;
volatile uint16_t whiteKelvin = KELVIN_DEFAULT;
volatile uint32_t pickedColor = 0xFF8000;
uint8_t onLevel = MAX_BRIGHTNESS;  // brightness used by "on"; set by the slider

Preferences prefs;
volatile bool settingsDirty = false;
volatile uint32_t settingsChangedAt = 0;

// White points tuned for WS2812B (their own white is bluish, so "warm" needs
// less blue than a light bulb chart would say). 4000 K is the original colour.
struct WhitePoint { uint16_t kelvin; uint8_t r, g, b; };
const WhitePoint WHITE_POINTS[] = {
    {2200, 255, 130, 30},
    {2700, 255, 160, 60},
    {3000, 255, 180, 90},
    {3500, 255, 205, 125},
    {4000, 255, 223, 150},
    {5000, 255, 238, 200},
    {6500, 255, 255, 255},
};

uint32_t kelvinToColor(uint16_t kelvin) {
    const size_t n = sizeof(WHITE_POINTS) / sizeof(WHITE_POINTS[0]);
    if (kelvin <= WHITE_POINTS[0].kelvin) {
        return (uint32_t)WHITE_POINTS[0].r << 16 | WHITE_POINTS[0].g << 8 | WHITE_POINTS[0].b;
    }
    for (size_t i = 1; i < n; i++) {
        const WhitePoint &a = WHITE_POINTS[i - 1];
        const WhitePoint &b = WHITE_POINTS[i];
        if (kelvin <= b.kelvin) {
            const uint32_t t = (uint32_t)(kelvin - a.kelvin) * 256 / (b.kelvin - a.kelvin);
            const uint8_t r = a.r + ((int)b.r - a.r) * (int)t / 256;
            const uint8_t g = a.g + ((int)b.g - a.g) * (int)t / 256;
            const uint8_t bl = a.b + ((int)b.b - a.b) * (int)t / 256;
            return (uint32_t)r << 16 | g << 8 | bl;
        }
    }
    return 0xFFFFFF;
}

void markSettingsChanged() {
    settingsChangedAt = millis();
    settingsDirty = true;
}

void applyColorSettings() {
    targetColor = lightMode == MODE_COLOR ? pickedColor : kelvinToColor(whiteKelvin);
}

void setWhite(uint16_t kelvin) {
    whiteKelvin = constrain(kelvin, KELVIN_MIN, KELVIN_MAX);
    lightMode = MODE_WHITE;
    applyColorSettings();
    markSettingsChanged();
}

void setColor(uint32_t rgb) {
    pickedColor = rgb & 0xFFFFFF;
    lightMode = MODE_COLOR;
    applyColorSettings();
    markSettingsChanged();
}

void loadSettings() {
    prefs.begin("light", true);
    lightMode = prefs.getUChar("mode", MODE_WHITE) == MODE_COLOR ? MODE_COLOR : MODE_WHITE;
    whiteKelvin = constrain(prefs.getUShort("kelvin", KELVIN_DEFAULT), KELVIN_MIN, KELVIN_MAX);
    pickedColor = prefs.getUInt("color", 0xFF8000) & 0xFFFFFF;
    onLevel = constrain(prefs.getUChar("level", MAX_BRIGHTNESS), 1, MAX_BRIGHTNESS);
    prefs.end();
    applyColorSettings();
    shownColor = targetColor;
}

// Saved a few seconds after the last change, so dragging a slider doesn't
// wear out the flash.
void saveSettingsIfDue(uint32_t now) {
    if (!settingsDirty || now - settingsChangedAt < 3000) {
        return;
    }
    settingsDirty = false;
    prefs.begin("light", false);
    prefs.putUChar("mode", lightMode);
    prefs.putUShort("kelvin", whiteKelvin);
    prefs.putUInt("color", pickedColor);
    prefs.putUChar("level", onLevel);
    prefs.end();
    Serial.println("settings saved");
}
#else
const uint8_t onLevel = MAX_BRIGHTNESS;
#endif

void renderLight(uint8_t brightnessLevel, uint32_t color) {
    const uint8_t baseR = color >> 16;
    const uint8_t baseG = (color >> 8) & 0xFF;
    const uint8_t baseB = color & 0xFF;

    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, RgbColor(baseR * brightnessLevel / 255, baseG * brightnessLevel / 255, baseB * brightnessLevel / 255));
    }
    strip.Show();
    shownBrightness = brightnessLevel;
    shownColor = color;
}

bool lightOn() {
    return targetBrightness > 0;
}

void setLight(bool on, const char *source) {
    targetBrightness = on ? onLevel : 0;
    Serial.printf("light %s (%s)\n", on ? "ON" : "OFF", source);
}

void toggleLight(const char *source) {
    setLight(!lightOn(), source);
}

void setBrightness(uint8_t level) {
#if MAX_BRIGHTNESS < 255
    if (level > MAX_BRIGHTNESS) {
        level = MAX_BRIGHTNESS;
    }
#endif
#if COLOR_CONTROL
    if (level > 0 && level != onLevel) {
        onLevel = level;
        markSettingsChanged();
    }
#endif
    targetBrightness = level;
}

int stepToward(int from, int to, int step) {
    if (from < to) {
        return min(from + step, to);
    }
    return max(from - step, to);
}

void updateStrip(uint32_t now) {
    const uint8_t target = targetBrightness;
    const uint32_t color = targetColor;
    if (target == shownBrightness && color == shownColor) {
        return;
    }
#if FADE_MS > 0
    if (now - lastFadeStepAt < FADE_STEP_MS) {
        return;
    }
    lastFadeStepAt = now;
    const int next = stepToward(shownBrightness, target, max(1, 255 * FADE_STEP_MS / FADE_MS));
#if COLOR_CONTROL
    const uint32_t nextColor =
        (uint32_t)stepToward(shownColor >> 16, color >> 16, COLOR_STEP) << 16 |
        (uint32_t)stepToward((shownColor >> 8) & 0xFF, (color >> 8) & 0xFF, COLOR_STEP) << 8 |
        (uint32_t)stepToward(shownColor & 0xFF, color & 0xFF, COLOR_STEP);
#else
    const uint32_t nextColor = color;
#endif
    renderLight(static_cast<uint8_t>(next), nextColor);
#else
    (void)now;
    renderLight(target, color);
#endif
}

// ------------------------------------------------------------------ button / sensor input

#if SENSOR_PIN >= 0
bool sensorStable = false;
bool sensorLastRaw = false;
uint32_t sensorRawSince = 0;
uint32_t lastSensorToggleAt = 0;

bool sensorActive(bool level) {
    return SENSOR_ACTIVE_HIGH ? level : !level;
}

void sensorBegin() {
#if SENSOR_PULL == 1
    pinMode(SENSOR_PIN, INPUT_PULLUP);
#elif SENSOR_PULL == 2
    pinMode(SENSOR_PIN, INPUT_PULLDOWN);
#else
    pinMode(SENSOR_PIN, INPUT);
#endif
    delay(5);
    // The level at power-up is the starting point, not a wave.
    sensorStable = sensorLastRaw = digitalRead(SENSOR_PIN);
    lastSensorToggleAt = millis() - SENSOR_LOCKOUT_MS;  // the first wave counts straight away
    Serial.printf("sensor on GPIO%d, now %s\n", SENSOR_PIN, sensorActive(sensorStable) ? "active" : "idle");
}

void sensorPoll(uint32_t now) {
    const bool raw = digitalRead(SENSOR_PIN);
    if (raw != sensorLastRaw) {
        sensorLastRaw = raw;
        sensorRawSince = now;
        return;
    }
    if (raw == sensorStable || now - sensorRawSince < SENSOR_DEBOUNCE_MS) {
        return;
    }
    sensorStable = raw;
    Serial.printf("sensor: %s\n", sensorActive(raw) ? "active" : "idle");

    // Pulse sensors: only the start of a pulse is a wave.
    // Latching sensors: every change of the output is a wave.
    const bool wave = SENSOR_LATCHING ? true : sensorActive(raw);
    if (!wave) {
        return;
    }
    // A pulse sensor can fire twice for one slow wave; a latching sensor only
    // flips on a real wave, so every flip counts.
    if (!SENSOR_LATCHING && now - lastSensorToggleAt < SENSOR_LOCKOUT_MS) {
        Serial.println("sensor: ignored (too soon after the last wave)");
        return;
    }
    lastSensorToggleAt = now;
    toggleLight(SENSOR_LABEL);
}
#endif

// ------------------------------------------------------------------ web

void addSecurityHeaders(AsyncWebServerResponse *response) {
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("X-Frame-Options", "DENY");
    response->addHeader("Referrer-Policy", "no-referrer");
}

void sendCaptiveRedirect(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "http://192.168.4.1/");
    addSecurityHeaders(response);
    request->send(response);
}

void sendRedirectHome(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(303);
    response->addHeader("Location", "/");
    addSecurityHeaders(response);
    request->send(response);
}

String stateJson() {
    String body = String("{\"ok\":true,\"lightOn\":") + (lightOn() ? "true" : "false") +
                  ",\"brightness\":" + String(targetBrightness);
#if COLOR_CONTROL
    char extra[96];
    snprintf(extra, sizeof(extra), ",\"level\":%u,\"mode\":\"%s\",\"kelvin\":%u,\"color\":\"#%06lx\"",
             (unsigned)onLevel, lightMode == MODE_COLOR ? "color" : "white", (unsigned)whiteKelvin,
             (unsigned long)pickedColor);
    body += extra;
#endif
    body += "}";
    return body;
}

// A parameter from the query string or a form body, or "" if missing.
String requestParam(AsyncWebServerRequest *request, const char *name) {
    if (request->hasParam(name, true)) {
        return request->getParam(name, true)->value();
    }
    if (request->hasParam(name)) {
        return request->getParam(name)->value();
    }
    return String();
}

void sendJson(AsyncWebServerRequest *request, int code, const String &body) {
    AsyncWebServerResponse *response = request->beginResponse(code, "application/json; charset=utf-8", body);
    addSecurityHeaders(response);
    request->send(response);
}

// Scripts (Accept: application/json) get the new state; plain HTML forms are
// sent back to the page.
void sendAfterChange(AsyncWebServerRequest *request) {
    if (request->hasHeader("Accept") && request->getHeader("Accept")->value().indexOf("application/json") >= 0) {
        sendJson(request, 200, stateJson());
    } else {
        sendRedirectHome(request);
    }
}

#if COLOR_CONTROL
// Brightness, white warmth and colour. Everything is loaded from
// /api/light/state, so the page itself is fixed text.
const char COLOR_PAGE[] = R"HTML(<!doctype html><html lang='en'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>)HTML" LOCATION_TITLE R"HTML(</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#000;color:#e2e8f0;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:1rem}
.card{width:100%;max-width:360px;background:#111827;border-radius:16px;padding:1.2rem;box-shadow:0 10px 25px rgba(0,0,0,.35)}
h1{margin:.2rem 0 1rem;font-size:1.2rem;text-align:center}
.status{display:block;text-align:center;font-weight:700;margin-bottom:1rem;padding:.55rem;border-radius:10px}
.status.on{background:#14532d;color:#bbf7d0}.status.off{background:#7f1d1d;color:#fecaca}
.row{display:grid;grid-template-columns:1fr 1fr;gap:.75rem;margin-bottom:1rem}
button{width:100%;padding:.8rem;border:none;border-radius:10px;font-size:1rem;font-weight:700;cursor:pointer}
.on-btn{background:#22c55e;color:#052e16}.off-btn{background:#ef4444;color:#450a0a}
.tab{background:#1f2937;color:#e2e8f0}.tab.sel{background:#e2e8f0;color:#111827}
label{display:flex;justify-content:space-between;font-size:.9rem;margin:.2rem 0 .4rem}
input[type=range]{width:100%;height:2rem;margin:0 0 1rem;accent-color:#e2e8f0}
#kv{-webkit-appearance:none;appearance:none;height:1.4rem;border-radius:1rem;background:linear-gradient(90deg,#ff821e,#ffdf96,#fff)}
#kv::-webkit-slider-thumb{-webkit-appearance:none;width:1.8rem;height:1.8rem;border-radius:50%;background:#111827;border:3px solid #e2e8f0}
input[type=color]{width:100%;height:3rem;border:none;border-radius:10px;background:none;padding:0;margin-bottom:.75rem}
.sw{display:grid;grid-template-columns:repeat(7,1fr);gap:.4rem}
.sw button{height:2.2rem;padding:0;border:2px solid #374151}
.hide{display:none}
</style></head><body><main class='card'><h1>)HTML" LOCATION_TITLE R"HTML(</h1>
<span id='st' class='status off'>Current state: ...</span>
<div class='row'><button class='on-btn' onclick="post('/api/light/on','')">Turn ON</button>
<button class='off-btn' onclick="post('/api/light/off','')">Turn OFF</button></div>
<label>Brightness <span id='bl'></span></label>
<input id='br' type='range' min='1' max='100'>
<div class='row'><button id='tw' class='tab' onclick='tab("white")'>White</button>
<button id='tc' class='tab' onclick='tab("color")'>Colour</button></div>
<div id='pw'><label><span>Warm</span><span id='kl'></span><span>Cool</span></label>
<input id='kv' type='range' min='2200' max='6500' step='100'></div>
<div id='pc' class='hide'><input id='cp' type='color' value='#ff8000'>
<div class='sw'></div></div>
</main><script>
var $=function(i){return document.getElementById(i)},S={},edit=0,timers={};
var SW=['#ff0000','#ff8000','#ffd000','#00ff40','#00d0ff','#0040ff','#c000ff'];
function show(s){S=s;var e=$('st');e.className='status '+(s.lightOn?'on':'off');
e.textContent='Current state: '+(s.lightOn?'ON':'OFF');
if(Date.now()-edit>3000){$('br').value=Math.max(1,Math.round(Math.sqrt(s.level/255)*100));
$('kv').value=s.kelvin;$('cp').value=s.color;mode(s.mode)}
$('bl').textContent=$('br').value+'%';$('kl').textContent=$('kv').value+' K'}
function post(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','Accept':'application/json'},body:d})
.then(function(r){return r.json()}).then(show).catch(function(){})}
function later(k,f){edit=Date.now();clearTimeout(timers[k]);timers[k]=setTimeout(f,120)}
function mode(m){$('tw').className='tab'+(m=='white'?' sel':'');$('tc').className='tab'+(m=='color'?' sel':'');
$('pw').className=m=='white'?'':'hide';$('pc').className=m=='color'?'':'hide'}
function tab(m){edit=Date.now();mode(m);if(m=='white')post('/api/light/white','kelvin='+$('kv').value);
else post('/api/light/color','hex='+$('cp').value.slice(1))}
$('br').oninput=function(){$('bl').textContent=this.value+'%';var v=this.value;
later('b',function(){post('/api/light/brightness','level='+Math.max(1,Math.round(255*v*v/10000)))})};
$('kv').oninput=function(){$('kl').textContent=this.value+' K';var v=this.value;
later('k',function(){post('/api/light/white','kelvin='+v)})};
$('cp').oninput=function(){var v=this.value.slice(1);later('c',function(){post('/api/light/color','hex='+v)})};
SW.forEach(function(c){var b=document.createElement('button');b.style.background=c;
b.onclick=function(){$('cp').value=c;edit=Date.now();post('/api/light/color','hex='+c.slice(1))};
document.querySelector('.sw').appendChild(b)});
function poll(){fetch('/api/light/state').then(function(r){return r.json()}).then(show).catch(function(){})}
poll();setInterval(poll,2000);
</script></body></html>)HTML";
#endif

String renderControlPage() {
    const bool on = lightOn();
    const char *statusText = on ? "ON" : "OFF";
    const char *statusClass = on ? "on" : "off";

    String html;
    html.reserve(2900);
    html += "<!doctype html><html lang='en'><head>";
    html += "<meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>" LOCATION_TITLE "</title>";
    html += "<style>";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#000000;color:#e2e8f0;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:1rem;}";
    html += ".card{width:100%;max-width:360px;background:#111827;border-radius:16px;padding:1.2rem;box-shadow:0 10px 25px rgba(0,0,0,.35);}";
    html += "h1{margin:.2rem 0 1rem;font-size:1.2rem;text-align:center;}";
    html += ".status{display:block;text-align:center;font-weight:700;margin-bottom:1rem;padding:.55rem;border-radius:10px;}";
    html += ".status.on{background:#14532d;color:#bbf7d0;}";
    html += ".status.off{background:#7f1d1d;color:#fecaca;}";
    html += ".actions{display:grid;grid-template-columns:1fr 1fr;gap:.75rem;}";
    html += "button{width:100%;padding:.8rem;border:none;border-radius:10px;font-size:1rem;font-weight:700;cursor:pointer;}";
    html += ".on-btn{background:#22c55e;color:#052e16;}";
    html += ".off-btn{background:#ef4444;color:#450a0a;}";
    html += "</style></head><body>";
    html += "<main class='card'><h1>" LOCATION_TITLE "</h1>";
    html += "<span id='st' class='status ";
    html += statusClass;
    html += "'>Current state: ";
    html += statusText;
    html += "</span>";
    html += "<div class='actions'>";
    html += "<form method='POST' action='/api/light/on'><button class='on-btn' type='submit'>Turn ON</button></form>";
    html += "<form method='POST' action='/api/light/off'><button class='off-btn' type='submit'>Turn OFF</button></form>";
    html += "</div></main>";
    // Keeps the status current when the button switches the light.
    html += "<script>setInterval(function(){fetch('/api/light/state').then(function(r){return r.json();}).then(function(s){"
            "var e=document.getElementById('st');e.className='status '+(s.lightOn?'on':'off');"
            "e.textContent='Current state: '+(s.lightOn?'ON':'OFF');}).catch(function(){});},2000);</script>";
    html += "</body></html>";

    return html;
}

// ------------------------------------------------------------------ setup / loop

void setup() {
#if DISABLE_BROWNOUT
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
#endif

    Serial.begin(115200);

    strip.Begin();
#if COLOR_CONTROL
    loadSettings();
#endif
    renderLight(0, targetColor);
    if (LIGHT_ON_AT_BOOT) {
        setLight(true, "power-up");
    }

#if SENSOR_PIN >= 0
    sensorBegin();
#endif

    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONNECTIONS);
    if (!apStarted) {
        Serial.println("Failed to start AP mode. Restarting...");
        delay(500);
        ESP.restart();
    }

    IPAddress apIp = WiFi.softAPIP();

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIp);

    Serial.println("AP mode started");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Open: http://");
    Serial.println(apIp);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
#if COLOR_CONTROL
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html; charset=utf-8", String(COLOR_PAGE));
#else
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html; charset=utf-8", renderControlPage());
#endif
        addSecurityHeaders(response);
        request->send(response);
    });

    // Phones probe these to detect a captive portal; redirecting opens the page.
    for (const char *probe : {"/hotspot-detect.html", "/generate_204", "/gen_204", "/redirect", "/connecttest.txt", "/ncsi.txt"}) {
        server.on(probe, HTTP_GET, [](AsyncWebServerRequest *request) {
            sendCaptiveRedirect(request);
        });
    }

    server.on("/api/light/on", HTTP_POST, [](AsyncWebServerRequest *request) {
        setLight(true, "web");
        sendAfterChange(request);
    });

    server.on("/api/light/off", HTTP_POST, [](AsyncWebServerRequest *request) {
        setLight(false, "web");
        sendAfterChange(request);
    });

    server.on("/api/light/toggle", HTTP_POST, [](AsyncWebServerRequest *request) {
        toggleLight("web");
        sendJson(request, 200, stateJson());
    });

    server.on("/api/light/state", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, stateJson());
    });

    server.on("/api/light/brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
        bool hasQueryLevel = request->hasParam("level");
        bool hasBodyLevel = request->hasParam("level", true);
        if (!hasQueryLevel && !hasBodyLevel) {
            sendJson(request, 400, "{\"error\":\"Missing level (0-255)\"}");
            return;
        }

        String levelParam = hasBodyLevel ? request->getParam("level", true)->value() : request->getParam("level")->value();
        int parsedLevel = constrain(levelParam.toInt(), 0, 255);
        setBrightness(static_cast<uint8_t>(parsedLevel));
        sendJson(request, 200, stateJson());
    });

#if COLOR_CONTROL
    server.on("/api/light/white", HTTP_POST, [](AsyncWebServerRequest *request) {
        const String kelvin = requestParam(request, "kelvin");
        if (kelvin.length() == 0) {
            sendJson(request, 400, "{\"error\":\"Missing kelvin (2200-6500)\"}");
            return;
        }
        setWhite(static_cast<uint16_t>(constrain(kelvin.toInt(), KELVIN_MIN, KELVIN_MAX)));
        sendJson(request, 200, stateJson());
    });

    server.on("/api/light/color", HTTP_POST, [](AsyncWebServerRequest *request) {
        String hex = requestParam(request, "hex");
        if (hex.startsWith("#")) {
            hex = hex.substring(1);
        }
        bool valid = hex.length() == 6;
        for (unsigned i = 0; valid && i < hex.length(); i++) {
            valid = isxdigit(static_cast<unsigned char>(hex[i]));
        }
        if (!valid) {
            sendJson(request, 400, "{\"error\":\"hex must be 6 hex digits, e.g. ff8000\"}");
            return;
        }
        setColor(strtoul(hex.c_str(), nullptr, 16));
        sendJson(request, 200, stateJson());
    });
#endif

    server.onNotFound([](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.begin();
}

void loop() {
    const uint32_t now = millis();
    dnsServer.processNextRequest();
#if SENSOR_PIN >= 0
    sensorPoll(now);
#endif
    updateStrip(now);
#if COLOR_CONTROL
    saveSettingsIfDue(now);
#endif
    delay(1);
}
