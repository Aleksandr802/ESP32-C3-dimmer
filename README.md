# ESP32 Wi-Fi LED light

One firmware for several rooms. Each board opens its own Wi-Fi network; join
it with a phone and the control page opens by itself (or go to
http://192.168.4.1). Pick the room when building:

| Build (env) | Board | LEDs | Wi-Fi network | Extra |
|---|---|---|---|---|
| `esp-32u` | ESP32 dev board | 22 on GPIO12 | `BathLight` | — |
| `kitchen_light` | ESP32-C3 SuperMini | 52 on GPIO4 | `KitchenLight` | Sweep Hands switch read on GPIO3, 0.6 s fade |

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
- Sweep Hands SW V2.0 hand-wave switch (power in, power out, no signal wire)
- 5 V 4 A power supply
- 330 Ω resistor, 1000 µF 10 V (or higher) capacitor
- 10 kΩ and 20 kΩ resistors (for the sensor)

### How the sensor is used

The Sweep Hands switch is made to cut a strip's power itself. Here it does
**not** carry the strip's power. It gets 5 V on its input, and its output only
goes to one input pin of the board. Every wave flips its output, and the board
toggles the light on each flip. The strip and the board are fed directly from
the supply, so the light also works from Wi-Fi whatever the switch's output
is doing.

### Step 1: find out which wire the switch turns on and off

Connect only the supply to the switch's **input** (+ and −). Nothing on its
output yet. Set a multimeter to DC volts, black probe on the supply's **GND**
(not the switch's output −), and wave a hand a few times:

| Red probe on | Reading | Your switch |
|---|---|---|
| output **+** | goes 5 V ↔ 0 V with each wave | switches the + wire: **wiring A** |
| output **+** | stays at 5 V | switches the − wire: **wiring B** (check: output − goes 0 V ↔ floating) |

### Step 2: wiring

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
         │  │   GPIO3 ── (see A or B)
         │  │
         │  │   Sweep Hands switch
         └──┼── input +
            └── input −
                output + / output −  ── (see A or B)
```

**Wiring A** (switches +): the output is 5 V, too much for the C3, so it goes
through a divider that brings it to 3.3 V:

```
switch output + ── 10 kΩ ──┬── GPIO3
                           └── 20 kΩ ── GND
switch output −: not connected
```

Settings: the defaults (`SENSOR_ACTIVE_HIGH=1`, `SENSOR_LATCHING=1`, no pull).

**Wiring B** (switches −): output − is pulled to GND when the switch is on.
The board's own pull-up keeps it at 3.3 V otherwise, so no resistors:

```
switch output − ── GPIO3
switch output +: not connected
```

Settings: in `[env:kitchen_light]` change to `-DSENSOR_ACTIVE_HIGH=0` and add
`-DSENSOR_PULL=1`.

Never connect the switch's output **+** straight to GPIO3: 5 V can damage the C3.

### Step 3: check

Open the serial monitor (`pio device monitor -b 115200`) and wave a hand.
Each wave should print one `sensor: active` or `sensor: idle` line, followed
by `light ON (hand sensor)` or `light OFF (hand sensor)`.

| You see | Fix |
|---|---|
| nothing | check the wiring; in wiring B make sure `SENSOR_PULL=1` is set |
| two lines per wave, light goes on and straight off | the switch gives a pulse, not a flip: set `-DSENSOR_LATCHING=0` |
| holding a hand there prints nothing, then one line | normal: some of these switches dim while a hand is held; the board ignores the dimming and only counts the final on/off |

### Notes on the rest of the wiring

- **One common ground**: supply, strip, board and switch input − all connected.
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
