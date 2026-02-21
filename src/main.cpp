#include <NeoPixelBus.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define LED_PIN 12
#define NUM_LEDS 22
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(NUM_LEDS, LED_PIN);
AsyncWebServer server(80);
DNSServer dnsServer;

void addSecurityHeaders(AsyncWebServerResponse *response);

const char *apSsid = "BathLight";
const char *apPassword = "LightCtrl987";

bool lightOn = false;
uint8_t currentBrightness = 0;

void sendCaptiveRedirect(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "http://192.168.4.1/");
    addSecurityHeaders(response);
    request->send(response);
}

void setWarmLight(uint8_t brightnessLevel) {
    const uint8_t baseR = 255;
    const uint8_t baseG = 223;
    const uint8_t baseB = 150;

    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, RgbColor(baseR * brightnessLevel / 255, baseG * brightnessLevel / 255, baseB * brightnessLevel / 255));
    }
    strip.Show();
    currentBrightness = brightnessLevel;
    lightOn = currentBrightness > 0;
}

void setWarmLight(bool on) {
    setWarmLight(static_cast<uint8_t>(on ? 255 : 0));
}

void fadeIn(uint16_t stepDelayMs = 60, uint8_t step = 5) {
    for (uint16_t level = currentBrightness; level <= 255; level += step) {
        setWarmLight(static_cast<uint8_t>(level));
        delay(stepDelayMs);
        yield();
        if (level >= (255 - step)) {
            break;
        }
    }
    setWarmLight(static_cast<uint8_t>(255));
}

void fadeOut(uint16_t stepDelayMs = 20, uint8_t step = 5) {
    for (int16_t level = currentBrightness; level >= 0; level -= step) {
        setWarmLight(static_cast<uint8_t>(level));
        delay(stepDelayMs);
        yield();
    }
    setWarmLight(static_cast<uint8_t>(0));
}

void addSecurityHeaders(AsyncWebServerResponse *response) {
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("X-Frame-Options", "DENY");
    response->addHeader("Referrer-Policy", "no-referrer");
}

String renderControlPage() {
    const char *statusText = lightOn ? "ON" : "OFF";
    const char *statusClass = lightOn ? "on" : "off";

    String html;
    html.reserve(2300);
    html += "<!doctype html><html lang='en'><head>";
    html += "<meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP32 Light Control</title>";
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
    html += "<main class='card'><h1>Living Room Light</h1>";
    html += "<span class='status ";
    html += statusClass;
    html += "'>Current state: ";
    html += statusText;
    html += "</span>";
    html += "<div class='actions'>";
    html += "<form method='POST' action='/api/light/on'><button class='on-btn' type='submit'>Turn ON</button></form>";
    html += "<form method='POST' action='/api/light/off'><button class='off-btn' type='submit'>Turn OFF</button></form>";
    html += "</div></main></body></html>";

    return html;
}

void sendRedirectHome(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(303);
    response->addHeader("Location", "/");
    addSecurityHeaders(response);
    request->send(response);
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector

    Serial.begin(115200);

    strip.Begin();
    setWarmLight(static_cast<uint8_t>(255)); // LED on at boot

    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

    bool apStarted = WiFi.softAP(apSsid, apPassword, AP_CHANNEL, 0, AP_MAX_CONNECTIONS);
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
    Serial.println(apSsid);
    Serial.print("Open: http://");
    Serial.println(apIp);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html; charset=utf-8", renderControlPage());
        addSecurityHeaders(response);
        request->send(response);
    });

    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/redirect", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.on("/api/light/on", HTTP_POST, [](AsyncWebServerRequest *request) {
        setWarmLight(true);
        sendRedirectHome(request);
    });

    server.on("/api/light/off", HTTP_POST, [](AsyncWebServerRequest *request) {
        setWarmLight(false);
        sendRedirectHome(request);
    });

    server.on("/api/light/brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
        bool hasQueryLevel = request->hasParam("level");
        bool hasBodyLevel = request->hasParam("level", true);
        if (!hasQueryLevel && !hasBodyLevel) {
            AsyncWebServerResponse *response = request->beginResponse(400, "application/json; charset=utf-8", "{\"error\":\"Missing level (0-255)\"}");
            addSecurityHeaders(response);
            request->send(response);
            return;
        }

        String levelParam = hasBodyLevel ? request->getParam("level", true)->value() : request->getParam("level")->value();
        int parsedLevel = levelParam.toInt();
        if (parsedLevel < 0) {
            parsedLevel = 0;
        }
        if (parsedLevel > 255) {
            parsedLevel = 255;
        }

        setWarmLight(static_cast<uint8_t>(parsedLevel));

        String body = "{\"ok\":true,\"lightOn\":" + String(lightOn ? "true" : "false") + ",\"brightness\":" + String(currentBrightness) + "}";
        AsyncWebServerResponse *response = request->beginResponse(200, "application/json; charset=utf-8", body);
        addSecurityHeaders(response);
        request->send(response);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        sendCaptiveRedirect(request);
    });

    server.begin();
}

void loop() {
    dnsServer.processNextRequest();
}
