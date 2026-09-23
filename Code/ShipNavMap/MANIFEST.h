// ============================================================
// MANIFEST.h - WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
//
// >>> TO CHANGE HOW MANY LEDS LIGHT PER LANDMARK: edit LEDS_PER_SEGMENT.
//     TO CHANGE THE STRIP LENGTH: edit LED_STRING_LENGTH.
//     TO CHANGE THE PIXEL-TO-PIXEL PAUSE: edit LED_STEP_MS. <<<
// ============================================================

#pragma once

#define DEVICE_NAME           "ShipNavMap"
#define FIRMWARE_VERSION      "2.1.3-count"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Ship-room navigational map LED strip. Stays DARK until Unreal starts the wheel/obstacle-course crossing (MermaidsTale/ObstacleCourseIntro = trigger, the same cue that makes Red Beard order a player to the wheel). That lights segment 1 (first 3 LEDs). Each landmark the helmsman reaches (MermaidsTale/Landmark1..4 = Triggered) lights the next 3-LED segment. Landmark5 = crossing complete (status SOLVED). Pixels inside a segment light ONE AT A TIME with a 1 s pause. GameReset / GameStart turn the strip dark again. Listens only - never publishes to game topics."
#define BUILD_STATUS          "bench"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS_MANIFEST 300000

// Over-the-air updates - MANDATORY on every Wi-Fi board (mqtt-protocol.md, 2026-09-22).
// Password = the Wi-Fi password (OTA_PASSWORD aliases WIFI_PASS in the sketch).
// After the one-time USB flash: arduino-cli upload -p <board IP> --upload-field password=<Wi-Fi password> ...
#define OTA_ENABLED           "yes"
#define OTA_HOSTNAME          "ShipNavMap"          // = DEVICE_NAME
#if !defined(ESP8266)
#define OTA_PORT              3232                  // ESP32 / S3 (first so the WatchTower parser reads it)
#else
#define OTA_PORT              8266
#endif

#define SUBSCRIBE_TOPICS      "MermaidsTale/ObstacleCourseIntro (trigger = light segment 1; 'speaking' ack ignored), MermaidsTale/Landmark1..5 (Triggered|true|1 = light next segment, Landmark5 = SOLVED), MermaidsTale/GameReset and MermaidsTale/GameStart (any payload = strip dark), MermaidsTale/ShipNavMap/command"
#define PUBLISH_TOPICS        "MermaidsTale/ShipNavMap/status (retained ONLINE|SOLVED, LWT OFFLINE, HEARTBEAT:STATE:UPxs:RSSIx every 5 min), MermaidsTale/ShipNavMap/progress (SEG:n/5 non-retained on every change), MermaidsTale/ShipNavMap/log, MermaidsTale/ShipNavMap/command (PONG/OK/STATUS replies)"
#define SUPPORTED_COMMANDS    "PING, STATUS, RESET (reboot), CLEAR (dark, same as GameReset), LIGHTS_TEST, PREVIEW <0-5|NEXT|FULL|OFF> (bench: fake the game progress without touching game topics), FILL <n|OFF> (light LEDs 1..n at 1/s), PIXEL <n|OFF> (light one LED by number), WALK [OFF] (one LED steps along the strip 1/s)"

// Game topics this board LISTENS to (owned by Unreal / M3 - NEVER publish here)
#define TOPIC_INTRO           "MermaidsTale/ObstacleCourseIntro"
#define TOPIC_LANDMARK_PREFIX "MermaidsTale/Landmark"      // + "1".."5"
#define TOPIC_GAME_RESET      "MermaidsTale/GameReset"
#define TOPIC_GAME_START      "MermaidsTale/GameStart"
#define LANDMARK_COUNT        5

// ------------------------------------------------------------
// Lights - one WS2812B strip. LED_STRING_LENGTH / LEDS_PER_SEGMENT
// segments. Segment 1 = intro cue, segment 2 = Landmark1, ... so with
// 15 LEDs and 3 per segment: intro, L1, L2, L3, L4 fill the strip and
// Landmark5 marks the crossing complete. A longer strip (18 LEDs) would
// give Landmark5 its own segment automatically.
// ------------------------------------------------------------
#if defined(ESP8266)
#define LED_DATA_PIN          2        // D4 on NodeMCU / Wemos D1 mini
#else
#define LED_DATA_PIN          16       // ESP32 / ESP32-S3 GPIO
#endif
#define LED_STRING_LENGTH     150      // TEMP for counting - set to the real strip length once markers are mapped
#define LEDS_PER_SEGMENT      3        // pixels lit per landmark
#define LED_STEP_MS           1000     // pause between pixels inside a segment
#define LED_COLOR_ORDER       GRB      // WS2812B is GRB; boot self-test shows R,G,B in order
#define LED_BRIGHTNESS        100      // 0-255 global cap (100 while the strip is powered from the board; raise once it has its own 5V supply)
#define ROUTE_COLOR_R         255      // colour of a lit route pixel
#define ROUTE_COLOR_G         40
#define ROUTE_COLOR_B         0

// Hardware
#define PIN_CONFIG            "LED_DATA=16 on ESP32 (GPIO2/D4 on ESP8266) -> WS2812B data-in"
#define COMPONENTS            "1x WS2812B 5V strip, 15 pixels (FastLED), single data line. No sensors, no other I/O."
#define KNOWN_QUIRKS          "Ignores every game-topic message for the first 1.5 s after (re)subscribing so a retained Landmark/Intro replay after a reboot cannot light the strip out of turn. Landmarks catch up: if Landmark3 arrives and segments 2-3 are still dark they all light in order (Unreal can fire markers out of sequence when the ship drifts near one). Repeats of an already-lit landmark are ignored. ObstacleCourseIntro also carries the AI's 'speaking' ack - only the exact word 'trigger' lights the strip. Original Feb-2026 sketch drove the strip from A0, which on ESP8266 is analog-in only and cannot drive WS2812 - use the pins above."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/ShipNavMap"
