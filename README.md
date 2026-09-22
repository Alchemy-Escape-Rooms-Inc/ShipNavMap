# ShipNavMap

Ship-room navigational map LED strip for *A Mermaid's Tale*. Lights-only board:
it listens to the game on MQTT and draws the route as the helmsman steers
marker to marker. It never publishes to a game topic.

Firmware: `Code/ShipNavMap/ShipNavMap.ino` + `MANIFEST.h` (v2.0.0).
Compiles for ESP32 / ESP32-S3 (`esp32:esp32:esp32s3`) and ESP8266
(`esp8266:esp8266:nodemcuv2`). Libraries: FastLED, PubSubClient.

## What the strip does

| Cue on the wire | Who sends it | Strip |
|---|---|---|
| (boot, GameStart, GameReset) | M3 / Unreal | dark |
| `MermaidsTale/ObstacleCourseIntro` = `trigger` | Unreal, the moment the crossing starts. Same cue that makes Red Beard send a player to the wheel. | segment 1 (LEDs 1-3) |
| `MermaidsTale/Landmark1` = `Triggered` | Unreal | segment 2 (LEDs 4-6) |
| `MermaidsTale/Landmark2` = `Triggered` | Unreal | segment 3 (LEDs 7-9) |
| `MermaidsTale/Landmark3` = `Triggered` | Unreal | segment 4 (LEDs 10-12) |
| `MermaidsTale/Landmark4` = `Triggered` | Unreal | segment 5 (LEDs 13-15) |
| `MermaidsTale/Landmark5` = `Triggered` | Unreal | crossing complete, status `SOLVED` |

Inside a segment the pixels light one at a time, one second apart.
`true`, `1`, `yes`, `on` are accepted alongside `Triggered`. The AI's
`speaking` ack on the intro topic is ignored. Empty payloads (retained-erase
sweeper) are ignored. Landmarks catch up if one is skipped and repeats are
ignored. Game topics are ignored for 1.5 s after (re)subscribing so a retained
replay after a reboot cannot light the strip out of turn.

## Own topics

- `MermaidsTale/ShipNavMap/status` retained `ONLINE` | `SOLVED`, LWT `OFFLINE`, 5-min `HEARTBEAT:STATE:UPxs:RSSIx`
- `MermaidsTale/ShipNavMap/progress` `SEG:n/5` on every change
- `MermaidsTale/ShipNavMap/log` human-readable events
- `MermaidsTale/ShipNavMap/command` `PING`, `STATUS`, `RESET`, `CLEAR`, `LIGHTS_TEST`, `PREVIEW <0-5|NEXT|FULL|OFF>`

## Bench test without a game

```
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/ShipNavMap/command -m "PREVIEW NEXT"   # intro, then L1, L2 ... each call
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/ShipNavMap/command -m "PREVIEW FULL"   # whole route
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/ShipNavMap/command -m "PREVIEW OFF"    # dark
```

Or fire the real cues by hand:

```
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/ObstacleCourseIntro -m trigger
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/Landmark1 -m Triggered
mosquitto_pub -h 10.1.10.115 -t MermaidsTale/GameReset -m triggered
```

## Wiring

- Data: GPIO16 on ESP32/S3, GPIO2 (D4) on ESP8266. Change `LED_DATA_PIN` in `MANIFEST.h`.
- WS2812B 5V strip, 15 pixels, common ground with the board. A 330 ohm series
  resistor on data and a 1000 uF cap across 5V/GND at the strip are good practice.
- Boot self-test flashes the whole strip RED, GREEN, BLUE. If RED shows as
  another colour, change `LED_COLOR_ORDER`.

## Strip length

`LED_STRING_LENGTH / LEDS_PER_SEGMENT` segments. With 15 LEDs and 3 per
segment the intro plus Landmarks 1-4 fill the strip and Landmark5 marks it
solved. An 18-LED strip would give Landmark5 its own segment with no code change.
