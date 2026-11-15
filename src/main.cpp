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
//   SENSOR_LOCKOUT_MS  pulse inputs: presses closer together than this count once (default 800)
//   DISABLE_BROWNOUT   1 = turn off the brown-out reset (bathroom board only)

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
#ifndef SENSOR_PULL
#define SENSOR_PULL 0
#endif
#ifndef DISABLE_BROWNOUT
#define DISABLE_BROWNOUT 0
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
// requests only set the target, loop() moves the strip towards it.
volatile uint8_t targetBrightness = 0;
uint8_t shownBrightness = 0;
uint32_t lastFadeStepAt = 0;

void addSecurityHeaders(AsyncWebServerResponse *response);

// ------------------------------------------------------------------ light

void renderWarmLight(uint8_t brightnessLevel) {
    const uint8_t baseR = 255;
    const uint8_t baseG = 223;
    const uint8_t baseB = 150;

    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, RgbColor(baseR * brightnessLevel / 255, baseG * brightnessLevel / 255, baseB * brightnessLevel / 255));
    }
    strip.Show();
    shownBrightness = brightnessLevel;
}

bool lightOn() {
    return targetBrightness > 0;
}

void setLight(bool on, const char *source) {
    targetBrightness = on ? MAX_BRIGHTNESS : 0;
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
    targetBrightness = level;
}

void updateStrip(uint32_t now) {
    const uint8_t target = targetBrightness;
    if (target == shownBrightness) {
        return;
    }
#if FADE_MS > 0
    if (now - lastFadeStepAt < FADE_STEP_MS) {
        return;
    }
    lastFadeStepAt = now;
    const int step = max(1, 255 * FADE_STEP_MS / FADE_MS);
    int next = shownBrightness + (target > shownBrightness ? step : -step);
    if ((target > shownBrightness && next > target) || (target < shownBrightness && next < target)) {
        next = target;
    }
    renderWarmLight(static_cast<uint8_t>(next));
#else
    (void)now;
    renderWarmLight(target);
#endif
}

// ------------------------------------------------------------------ hand sensor

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
    toggleLight("hand sensor");
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
    return String("{\"ok\":true,\"lightOn\":") + (lightOn() ? "true" : "false") +
           ",\"brightness\":" + String(targetBrightness) + "}";
}

void sendJson(AsyncWebServerRequest *request, int code, const String &body) {
    AsyncWebServerResponse *response = request->beginResponse(code, "application/json; charset=utf-8", body);
    addSecurityHeaders(response);
    request->send(response);
}

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
    // Keeps the status current when the hand sensor switches the light.
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
    renderWarmLight(0);
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
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html; charset=utf-8", renderControlPage());
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
        sendRedirectHome(request);
    });

    server.on("/api/light/off", HTTP_POST, [](AsyncWebServerRequest *request) {
        setLight(false, "web");
        sendRedirectHome(request);
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
    delay(1);
}
