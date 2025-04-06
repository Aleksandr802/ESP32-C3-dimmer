Collecting workspace informationHere’s a project description you can use for your GitHub repository:

---

# ESP32-C3 Dimmer with NeoPixel LED Strip

This project is an ESP32-C3-based dimmer for controlling a WS2812 NeoPixel LED strip. It includes functionality for smooth brightness transitions and warm light effects. The project is built using the Arduino framework and PlatformIO.

## Features

- **Smooth Brightness Control**: Gradual fade-in effect for a warm light ambiance.
- **NeoPixel LED Strip Support**: Controls WS2812 LEDs with customizable brightness and color.
- **WiFi Connectivity**: (Commented out in the current version) Includes setup for connecting to WiFi and hosting an asynchronous web server.
- **Async Web Server**: (Optional) Can be used to control the LED strip remotely.

## Hardware Requirements

- ESP32-C3 DevKitM-1
- WS2812 NeoPixel LED strip
- Power supply for the LED strip
- Connection wires

## Software Requirements

- [PlatformIO](https://platformio.org/) IDE or extension for Visual Studio Code
- Arduino framework

## Getting Started

1. Clone the repository:
   ```bash
   git clone https://github.com/Aleksandr802/esp32-c3-dimmer.git
   cd esp32-c3-dimmer
   ```

2. Open the project in [PlatformIO](https://platformio.org/).

3. Connect your ESP32-C3 board to your computer.

4. Update the WiFi credentials in main.cpp:
   ```cpp
   const char* ssid = "Your_SSID";
   const char* password = "Your_PASSWORD";
   ```

5. Build and upload the firmware:
   ```bash
   pio run --target upload
   ```

6. Power up the NeoPixel LED strip and enjoy the warm light effects.

## Future Enhancements

- Add fade-out functionality.
- Enable WiFi and web server for remote control.
- Add support for dynamic color changes via web interface.

## License
