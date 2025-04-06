#include <NeoPixelBus.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#define LED_PIN 2       // Pin connected to WS2812 LED strip
#define NUM_LEDS 22     // Number of LEDs in the strip

// Define LED strip object
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(NUM_LEDS, LED_PIN);

// WiFi Credentials
const char* ssid = " ";
const char* password = " ";

// Create Async Web Server
AsyncWebServer server(80);

// Global brightness level
int brightness = 0;

// Function to set all LEDs to a color with the current brightness
void setWarmLight(uint8_t brightnessLevel) {
    uint8_t r = 255;
    uint8_t g = 223;
    uint8_t b = 150;
    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, RgbColor(r * brightnessLevel / 255, g * brightnessLevel / 255, b * brightnessLevel / 255));
    }
    strip.Show();
}

// Function to gradually increase brightness
void fadeIn() {
    for (int level = 0; level <= 255; level += 5) {  // Gradual increase
        setWarmLight(level);
        delay(60);
    }
    brightness = 255;
}

// Function to gradually decrease brightness
// void fadeOut() {
//     for (int level = 255; level >= 0; level -= 5) {  // Gradual decrease
//         setWarmLight(level);
//         delay(20);
//     }
//     brightness = 0;
// }

void setup() {
    Serial.begin(115200);
    
    // Initialize LED strip
    strip.Begin();
    strip.Show(); // Turn off all LEDs initially

    // Connect to WiFi
    // WiFi.begin(ssid, password);
    // Serial.print("Connecting to WiFi...");
    // while (WiFi.status() != WL_CONNECTED) {
    //     delay(500);
    //     Serial.print(".");
    // }
    // Serial.println("\nConnected to WiFi");
    // Serial.print("IP Address: ");
    // Serial.println(WiFi.localIP());

    // Start the server
    // server.begin();
    
    // Start fade-in effect at power-up
    fadeIn();
    // delay(2000); // Keep LEDs on for a while before fading out
    // fadeOut();
}

void loop() {
    // No action needed in the loop
}
