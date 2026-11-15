# ESP32 Wi-Fi LED light

One firmware for several rooms. Each board opens its own Wi-Fi network; join
it with a phone and the control page opens by itself (or go to
http://192.168.4.1). Pick the room when building:

| Build (env) | Board | LEDs | Wi-Fi network | Extra |
|---|---|---|---|---|
| `esp-32u` | ESP32 dev board | 22 on GPIO12 | `BathLight` | — |
| `kitchen_light` | ESP32-C3 SuperMini | 52 on GPIO4 | `KitchenLight` | hand sensor on GPIO3, 0.6 s fade |

```bash
pio run -e kitchen_light -t upload     # or choose the env in VS Code's PlatformIO toolbar
pio device monitor -b 115200
```

## Kitchen light

### Before the first build: the Wi-Fi password

This repository is public, so the kitchen password is not in it:

```bash
cp include/secrets_kitchen.h.example include/secrets_kitchen.h
```

Edit `include/secrets_kitchen.h` and set `AP_PASSWORD` (at least 8
characters). git ignores that file. Without it the build stops with a message
saying so.

### Parts

- ESP32-C3 SuperMini
- WS2812B 5 V strip, 52 LEDs
- Sweep Hands SW V2.0 hand-wave sensor
- 5 V 4 A power supply
- 330 Ω resistor, 1000 µF 10 V (or higher) capacitor
- For the sensor, if its output is 5 V: 10 kΩ and 20 kΩ resistors

### Wiring

```
 5 V 4 A supply                         WS2812B strip (52)
   +5V ──┬──────────────────────────────── 5V
         │        ┌── 1000 µF ──┐
   GND ──┼──┬─────┴─────────────┴──────── GND
         │  │
         │  │   ESP32-C3 SuperMini
         ├──┼── 5V
         │  ├── GND
         │  │   GPIO4 ── 330 Ω ────────── DIN   (data wire under 30 cm)
         │  │   GPIO3 ──┬── 10 kΩ ─┐
         │  │           └── 20 kΩ ─┼─ GND        (only if the sensor output is 5 V)
         │  │                     │
         │  │   Sweep Hands SW V2.0
         └──┼── VCC (+)            │
            └── GND (−)            │
                OUT ───────────────┘
```

- **One common ground**: supply, strip, board and sensor GND all connected.
- **Capacitor** across 5V/GND at the strip's input, minus side to GND.
- **Power**: 52 LEDs at full warm white draw about 2.6 A, so 4 A is enough.
  Use 0.5–0.75 mm² (20–18 AWG) wire for 5 V and GND. If the far end looks
  yellower or dimmer, also connect 5 V and GND to the far end of the strip.
- **Data level**: the C3 sends 3.3 V to a 5 V strip. That usually works with a
  short data wire. If the first LED flickers or shows random colours, add a
  74AHCT125 level shifter between GPIO4 and DIN.
- **Flashing over USB-C** while the 5 V supply is connected can push current
  between the two supplies. Unplug the supply's 5 V from the board while
  flashing (or flash before wiring it).

### The sensor: check its output first

The C3's pins take at most 3.3 V. Before connecting OUT to GPIO3, power the
sensor and measure OUT against GND with a multimeter, with and without a hand
in front of it:

- highest reading about 3.3 V or less: connect OUT straight to GPIO3, no resistors
- about 5 V: use the 10 kΩ / 20 kΩ divider shown above
- no change at all (stays near 0 V): the output may be open-collector; set
  `-DSENSOR_PULL=1` in `platformio.ini` and connect straight to GPIO3

If the sensor has **4 wires for power in and power out** (made to cut the
strip's power itself) and no signal wire, it can't be read by the board. It
would switch the board off along with the strip.

### The sensor: match the firmware to it

Open the serial monitor (`pio device monitor -b 115200`) and wave a hand once:

| You see | The sensor | Setting in `[env:kitchen_light]` |
|---|---|---|
| `sensor: active`, then `sensor: idle` soon after | gives a pulse per wave | default (`SENSOR_ACTIVE_HIGH=1`, `SENSOR_LATCHING=0`) |
| `sensor: idle`, then `sensor: active` soon after | pulse, active low | `-DSENSOR_ACTIVE_HIGH=0` |
| one line per wave, staying until the next wave | flips its output each wave | `-DSENSOR_LATCHING=1` |
| nothing | not reaching GPIO3 | check wiring, the voltage above, try `-DSENSOR_PULL=1` |

Every switch also prints `light ON (hand sensor)` or `light ON (web)`.

### Other settings

In `[env:kitchen_light]` of `platformio.ini`: `NUM_LEDS`, `LED_PIN`,
`SENSOR_PIN`, `FADE_MS` (0 = instant), `MAX_BRIGHTNESS` (0–255), and
`LIGHT_ON_AT_BOOT` (default 1: the light comes on when power returns, as in
the bathroom; set 0 to keep it off after a power cut).

Avoid GPIO2, GPIO8 and GPIO9 on the SuperMini: they decide how the board starts.

## API

| Request | Does |
|---|---|
| `GET /api/light/state` | `{"ok":true,"lightOn":true,"brightness":255}` |
| `POST /api/light/on`, `POST /api/light/off` | switch, then back to the page |
| `POST /api/light/toggle` | switch over, returns the state |
| `POST /api/light/brightness?level=0..255` | set brightness, returns the state |
