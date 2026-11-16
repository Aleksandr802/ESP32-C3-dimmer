# ESP32 Wi-Fi LED light

One firmware for several rooms. Each board opens its own Wi-Fi network; join
it with a phone and the control page opens by itself (or go to
http://192.168.4.1). Pick the room when building:

| Build (env) | Board | LEDs | Wi-Fi network | Extra |
|---|---|---|---|---|
| `esp-32u` | ESP32 dev board | 22 on GPIO12 | `BathLight` | — |
| `kitchen_light` | ESP32-C3 SuperMini | 52 on GPIO4 | `KitchenLight` | push button on GPIO3, brightness, warm/cool white and colours, 0.6 s fade |

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
- A push button (momentary, normally open)
- 5 V 4 A power supply
- 330 Ω resistor, 1000 µF 10 V (or higher) capacitor

### Wiring

```
 5 V 4 A supply                         WS2812B strip (52)
   +5V ──┬──────────────────────────────── 5V
         │        ┌── 1000 µF ──┐
   GND ──┼──┬─────┴─────────────┴──────── GND
         │  │
         │  │   ESP32-C3 SuperMini
         └──┼── 5V
            ├── GND
            │   GPIO4 ── 330 Ω ────────── DIN   (data wire under 30 cm)
            │   GPIO3 ──┐
            │         button
            └───────────┘                        (button between GPIO3 and GND)
```

The button needs no resistors: the board's own pull-up holds GPIO3 high, and
a press connects it to GND. Each press toggles the light (with the same fade
as the phone). Presses closer together than 0.3 s count once
(`SENSOR_LOCKOUT_MS`), and contact bounce is filtered out. Holding the button
toggles once.

A long wire to the button (several metres) can pick up interference; if the
light ever switches by itself, add a 100 nF capacitor between GPIO3 and GND
at the board.

### Check

Open the serial monitor (`pio device monitor -b 115200`). At start-up it
prints `sensor on GPIO3, now idle`. Each press prints `sensor: active`,
`light ON (button)` / `light OFF (button)`, then `sensor: idle`.

| You see | Fix |
|---|---|
| `now active` at start-up, nothing on presses | the button is wired between GPIO3 and 3.3 V or 5 V; it must go to GND |
| nothing at all | check both button wires; with a 4-leg button use two legs on opposite sides |

### Other inputs

The same input also takes a latching switch (a wall switch that stays on or
off: set `-DSENSOR_LATCHING=1`, every flip toggles) or a sensor with a signal
output. A sensor that only switches power, like the Sweep Hands SW V2.0, did
not work here.

### Notes on the rest of the wiring

- **One common ground**: supply, strip and board GND all connected.
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

### Brightness, white and colour

The kitchen page has a brightness slider, a **White** mode with a warm↔cool
slider (2200–6500 K; 4000 K is the original warm white) and a **Colour** mode
with a colour picker and seven preset colours. Changes crossfade in about
0.2 s. The phone, the button and the page all share the same settings: "on"
always comes back at the chosen brightness and colour.

The settings are saved on the board 3 seconds after the last change, so they
survive a power cut. This is on for the kitchen only (`-DCOLOR_CONTROL=1`).

Power: full-brightness cool white (6500 K) is the most demanding setting,
about 3.1 A for 52 LEDs, still within the 4 A supply.

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
| `POST /api/light/brightness?level=0..255` | set brightness (0 = off), returns the state |
| `POST /api/light/white?kelvin=2200..6500` | white at that warmth (kitchen) |
| `POST /api/light/color?hex=ff8000` | that colour (kitchen) |

With `COLOR_CONTROL` the state also has `level` (brightness used by "on"),
`mode` (`white`/`color`), `kelvin` and `color` (`#rrggbb`). Parameters can be
in the query string or a form body. `on`/`off` return the state as JSON when
the request has `Accept: application/json`, otherwise they redirect to the page.
