//================================================
//  A Mermaid's Tale - Ship Navigational Map LED strip (v2.0.1)
//  Target: ESP32 / ESP32-S3 (default) or ESP8266 - picked by the board
//  you compile for; pins come from MANIFEST.h.
//
//  WHAT THIS BOARD IS: a lights-only display for the ship-room nav map.
//  It reads nothing and never publishes to a game topic. It only LISTENS
//  to what Unreal / M3 already put on the wire and paints the route.
//
//  THE STORY IT TELLS
//    dark ............ game not at the crossing yet
//    segment 1 lit ... MermaidsTale/ObstacleCourseIntro = trigger
//                      (Unreal fires this the moment the crossing starts;
//                       it is the SAME cue that makes Red Beard send one
//                       player to the wheel and explain the markers)
//    segment 2 lit ... MermaidsTale/Landmark1 = Triggered
//    segment 3 lit ... MermaidsTale/Landmark2 = Triggered
//    segment 4 lit ... MermaidsTale/Landmark3 = Triggered
//    segment 5 lit ... MermaidsTale/Landmark4 = Triggered
//    SOLVED .......... MermaidsTale/Landmark5 = Triggered (strip full,
//                      status -> SOLVED)
//    dark again ...... MermaidsTale/GameReset or GameStart (any payload)
//
//  Inside a segment the pixels light ONE AT A TIME with LED_STEP_MS
//  (1 s) between them - the strip "draws" the route toward the island.
//
//  PAYLOADS ON THE WIRE (from WatchTower mqtt_*.txt, 2026-09):
//    ObstacleCourseIntro | trigger       <- Unreal   (lights segment 1)
//    ObstacleCourseIntro | speaking      <- AI ack   (ignored)
//    Landmark4           | Triggered     <- Unreal   (Triggered/true/1 all accepted)
//    GameReset           | triggered     <- M3       (dark)
//    <any game topic>    | ""            <- retained-erase sweeper (ignored)
//
//  SAFETY: landmarks CATCH UP (Landmark3 with segments 2-3 dark lights
//  both, in order) because Unreal can trip markers out of sequence when
//  the ship drifts near one. Repeats are ignored. Game topics are ignored
//  for the first 1.5 s after (re)subscribing so retained replays after a
//  reboot cannot light the strip mid-lobby.
//
//  OWN TOPICS
//    MermaidsTale/ShipNavMap/status    retained ONLINE|SOLVED, LWT OFFLINE, 5-min heartbeat
//    MermaidsTale/ShipNavMap/progress  SEG:n/5 on every change (non-retained)
//    MermaidsTale/ShipNavMap/log       human-readable events
//    MermaidsTale/ShipNavMap/command   PING, STATUS, RESET, CLEAR, LIGHTS_TEST,
//                                      PREVIEW <0-5|NEXT|FULL|OFF>,
//                                      PIXEL <n|OFF>, WALK [OFF]  (bench: find marker LEDs)
//
//  Fleet hardening (same as MiniBarrelsLights v1.0.0): LWT retained
//  OFFLINE, 30 s task WDT (ESP32), 2-min offline self-reboot,
//  non-blocking MQTT retry, one FastLED.show() per change.
//================================================

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
  #include <esp_task_wdt.h>
#endif
#include <PubSubClient.h>
#include <FastLED.h>
#include <ArduinoOTA.h>   // MANDATORY per mqtt-protocol.md (2026-09-22): wireless re-flash
#include <stdarg.h>
#include "MANIFEST.h"   // single source of truth: version, broker, pins, segment size

#define VERSION    FIRMWARE_VERSION
#define PROP_NAME  DEVICE_NAME
#define TOPIC_BUF  48

const unsigned long HEARTBEAT_MS      = HEARTBEAT_MS_MANIFEST;
const uint32_t      WDT_TIMEOUT_S     = 30;      // loop() stall -> panic reboot (ESP32)
const unsigned long WIFI_WAIT_MS      = 10000;   // per ensureWiFi() attempt
const unsigned long MQTT_RETRY_MS     = 2000;    // min gap between connect attempts
const unsigned long OFFLINE_REBOOT_MS = 120000;  // no broker for 2 min -> restart
const unsigned long SETTLE_MS         = 1500;    // ignore game topics this long after subscribe

// Lights ----------------------------------------------------------------
#define LED_COUNT      LED_STRING_LENGTH
#define SEGMENT_COUNT  (LED_STRING_LENGTH / LEDS_PER_SEGMENT)
static_assert(LED_STRING_LENGTH >= 1, "LED_STRING_LENGTH must be at least 1");
static_assert(LEDS_PER_SEGMENT >= 1, "LEDS_PER_SEGMENT must be at least 1");
static_assert(SEGMENT_COUNT >= 1, "strip shorter than one segment");

CRGB leds[LED_COUNT];
static const CRGB COLOR_OFF   = CRGB(0, 0, 0);
static const CRGB COLOR_ROUTE = CRGB(ROUTE_COLOR_R, ROUTE_COLOR_G, ROUTE_COLOR_B);

// Route state -----------------------------------------------------------
// segmentsWanted = how many segments the game says should be lit.
// pixelsLit      = how many pixels are actually on right now; the animator
//                  walks pixelsLit up toward segmentsWanted*LEDS_PER_SEGMENT
//                  one pixel per LED_STEP_MS.
uint8_t       segmentsWanted   = 0;
uint16_t      pixelsLit        = 0;
bool          crossingStarted  = false;   // intro seen this game
bool          landmarkSeen[LANDMARK_COUNT] = { false };
bool          solved           = false;   // Landmark5 seen
unsigned long lastStepMs       = 0;
bool          lightsDirty      = true;

// Bench counting helpers (PIXEL n / WALK) - override the route picture.
int           previewPixel     = -1;      // 0-based single lit LED, -1 = off
unsigned long previewPixelEnd  = 0;
bool          walkActive       = false;
uint16_t      walkIndex        = 0;       // 0-based LED currently lit by WALK
unsigned long walkNextMs       = 0;
const unsigned long PIXEL_HOLD_MS = 120000;   // PIXEL n stays lit 2 min
static const CRGB COLOR_MARK   = CRGB(255, 255, 255);

// WiFi + MQTT -----------------------------------------------------------
static const char* WIFI_SSID   = "AlchemyGuest";
static const char* WIFI_PASS   = "VoodooVacation5601";
static const char* MQTT_SERVER = BROKER_IP;
static const int   MQTT_PORT   = BROKER_PORT;
static const char* OTA_PASSWORD = WIFI_PASS;   // protocol: OTA password = Wi-Fi password

#define TOPIC_ROOT "MermaidsTale/ShipNavMap/"
static const char* MQTT_TOPIC_STATUS   = TOPIC_ROOT "status";
static const char* MQTT_TOPIC_PROGRESS = TOPIC_ROOT "progress";
static const char* MQTT_TOPIC_LOG      = TOPIC_ROOT "log";
static const char* MQTT_TOPIC_COMMAND  = TOPIC_ROOT "command";

static char landmarkTopic[LANDMARK_COUNT][TOPIC_BUF];   // MermaidsTale/Landmark1..5

WiFiClient   espClient;
PubSubClient mqtt(espClient);

unsigned long lastHeartbeat     = 0;
unsigned long lastMqttOkMs      = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long subscribedAtMs    = 0;

//================================================
//            Small helpers
//================================================
static void wdtFeed() {
#if !defined(ESP8266)
  esp_task_wdt_reset();
#else
  yield();
#endif
}

void mqttLogf(const char* format, ...) {
  char buffer[160];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (mqtt.connected()) mqtt.publish(MQTT_TOPIC_LOG, buffer);
  Serial.println(buffer);
}

// Triggered / true / 1 / yes / on - the AI program accepts the same set.
static bool isTruthy(const char* s) {
  return strcasecmp(s, "Triggered") == 0 || strcasecmp(s, "trigger") == 0 ||
         strcasecmp(s, "true") == 0      || strcmp(s, "1") == 0 ||
         strcasecmp(s, "yes") == 0       || strcasecmp(s, "on") == 0;
}

static uint16_t pixelsWanted() {
  uint32_t n = (uint32_t)segmentsWanted * LEDS_PER_SEGMENT;
  return n > LED_COUNT ? LED_COUNT : (uint16_t)n;
}

//================================================
//            Route logic
//================================================
void publishProgress() {
  char buf[48];
  snprintf(buf, sizeof(buf), "SEG:%u/%u%s", (unsigned)segmentsWanted,
           (unsigned)SEGMENT_COUNT, solved ? ":SOLVED" : "");
  if (mqtt.connected()) mqtt.publish(MQTT_TOPIC_PROGRESS, buf);
}

// Ask for `want` segments lit. Never goes backwards - use routeClear().
void wantSegments(uint8_t want, const char* why) {
  if (want > SEGMENT_COUNT) want = SEGMENT_COUNT;
  if (want <= segmentsWanted) return;
  segmentsWanted = want;
  lastStepMs     = millis() - LED_STEP_MS;   // first pixel lights on the next loop, no wait
  mqttLogf("%s -> segment %u of %u (pixels %u..%u lighting 1/s)", why,
           (unsigned)segmentsWanted, (unsigned)SEGMENT_COUNT,
           (unsigned)pixelsLit + 1, (unsigned)pixelsWanted());
  publishProgress();
}

void routeClear(const char* why) {
  bool wasLit = (pixelsLit > 0 || segmentsWanted > 0 || solved || crossingStarted);
  segmentsWanted  = 0;
  pixelsLit       = 0;
  crossingStarted = false;
  solved          = false;
  for (uint8_t i = 0; i < LANDMARK_COUNT; i++) landmarkSeen[i] = false;
  lightsDirty = true;
  if (wasLit) {
    mqttLogf("%s -> strip DARK, route cleared", why);
    if (mqtt.connected()) mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
    publishProgress();
  }
}

void onIntro() {
  if (crossingStarted) { mqttLogf("ObstacleCourseIntro repeat ignored"); return; }
  crossingStarted = true;
  wantSegments(1, "Crossing started (ObstacleCourseIntro=trigger)");
}

// Landmark n (1-based). Segment for Landmark n is n+1 (segment 1 = intro).
// Catch-up: lights every segment up to that one, in order.
void onLandmark(uint8_t n) {
  if (n < 1 || n > LANDMARK_COUNT) return;
  if (landmarkSeen[n - 1]) { mqttLogf("Landmark%u repeat ignored", (unsigned)n); return; }
  landmarkSeen[n - 1] = true;
  crossingStarted = true;             // a landmark implies the crossing began
  char why[32];
  snprintf(why, sizeof(why), "Landmark%u", (unsigned)n);
  wantSegments(n + 1, why);
  if (n == LANDMARK_COUNT && !solved) {
    solved = true;
    mqttLogf("Landmark%u -> crossing COMPLETE, route full", (unsigned)n);
    if (mqtt.connected()) mqtt.publish(MQTT_TOPIC_STATUS, "SOLVED", true);
    publishProgress();
  }
}

// One pixel per LED_STEP_MS until the strip matches what the game wants.
void serviceAnimation() {
  uint16_t want = pixelsWanted();
  if (pixelsLit >= want) return;
  unsigned long now = millis();
  if (now - lastStepMs < LED_STEP_MS) return;
  lastStepMs = now;
  pixelsLit++;
  lightsDirty = true;
  if (pixelsLit == want)
    Serial.printf("[LED] segment %u complete (%u pixels lit)\n",
                  (unsigned)segmentsWanted, (unsigned)pixelsLit);
}

//================================================
//            Lights
//================================================
void renderLights() {
  if (walkActive || previewPixel >= 0) {
    int mark = walkActive ? (int)walkIndex : previewPixel;
    for (uint16_t i = 0; i < LED_COUNT; i++)
      leds[i] = ((int)i == mark) ? COLOR_MARK : COLOR_OFF;
  } else {
    for (uint16_t i = 0; i < LED_COUNT; i++)
      leds[i] = (i < pixelsLit) ? COLOR_ROUTE : COLOR_OFF;
  }
  FastLED.show();
  lightsDirty = false;
}

// PIXEL n timeout + WALK stepper (bench only).
void serviceBenchHelpers() {
  unsigned long now = millis();
  if (previewPixel >= 0 && (long)(now - previewPixelEnd) >= 0) {
    previewPixel = -1;
    lightsDirty  = true;
  }
  if (walkActive && (long)(now - walkNextMs) >= 0) {
    walkNextMs = now + LED_STEP_MS;
    walkIndex++;
    if (walkIndex >= LED_COUNT) {
      walkActive  = false;
      lightsDirty = true;
      mqttLogf("WALK done at LED %u", (unsigned)LED_COUNT);
      return;
    }
    lightsDirty = true;
    if ((walkIndex + 1) % 5 == 0) mqttLogf("WALK at LED %u", (unsigned)walkIndex + 1);
    else Serial.printf("[WALK] LED %u\n", (unsigned)walkIndex + 1);
  }
}

// Boot / LIGHTS_TEST: whole strip RED, GREEN, BLUE, then back to the real
// state. If "RED" shows as another colour, change LED_COLOR_ORDER.
void lightsSelfTest() {
  const CRGB seq[3] = { CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255) };
  for (uint8_t k = 0; k < 3; k++) {
    fill_solid(leds, LED_COUNT, seq[k]);
    FastLED.show();
    delay(300);
    wdtFeed();
  }
  lightsDirty = true;
}

//================================================
//            WiFi + MQTT
//================================================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_WAIT_MS) {
    wdtFeed();
    delay(200);
  }
}

// Over-the-air updates (MANDATORY, mqtt-protocol.md 2026-09-22). After the
// one-time USB flash, re-flash over Wi-Fi:
//   arduino-cli upload --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc -p <board IP> \
//       --upload-field password=VoodooVacation5601 Code/ShipNavMap
// Board IP is in every STATUS reply and the boot line on /log.
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    walkActive = false; previewPixel = -1;          // nothing to stop but the bench helpers
    mqttLogf("OTA update starting - lights hold, back in ~30 s");
  });
  ArduinoOTA.onEnd([]()   { Serial.println("OTA done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", (unsigned)e); });
  ArduinoOTA.begin();
  Serial.printf("OTA ready: %s @ %s:%d\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str(), OTA_PORT);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = millis();
  String clientId = String(PROP_NAME) + "-" + String(random(0xffff), HEX);
  if (mqtt.connect(clientId.c_str(), MQTT_TOPIC_STATUS, 0, true, "OFFLINE")) {
    mqtt.subscribe(MQTT_TOPIC_COMMAND);
    mqtt.subscribe(TOPIC_INTRO);
    for (uint8_t i = 0; i < LANDMARK_COUNT; i++) mqtt.subscribe(landmarkTopic[i]);
    mqtt.subscribe(TOPIC_GAME_RESET);
    mqtt.subscribe(TOPIC_GAME_START);
    subscribedAtMs = millis();
    mqtt.publish(MQTT_TOPIC_STATUS, solved ? "SOLVED" : "ONLINE", true);
    mqttLogf("%s v%s online at IP %s (OTA :%d) - %u LEDs, %u per segment, %u segments",
             PROP_NAME, VERSION, WiFi.localIP().toString().c_str(), OTA_PORT,
             (unsigned)LED_COUNT, (unsigned)LEDS_PER_SEGMENT, (unsigned)SEGMENT_COUNT);
  } else {
    Serial.printf("MQTT failed rc=%d\n", mqtt.state());
  }
}

void promptStatus() {
  char buf[144];
  snprintf(buf, sizeof(buf), "%s|SEG:%u/%u|LIT:%u/%u|L%d%d%d%d%d|UP:%lus|IP:%s|OTA:%d|V%s",
           solved ? "SOLVED" : (crossingStarted ? "CROSSING" : "IDLE"),
           (unsigned)segmentsWanted, (unsigned)SEGMENT_COUNT,
           (unsigned)pixelsLit, (unsigned)LED_COUNT,
           landmarkSeen[0], landmarkSeen[1], landmarkSeen[2], landmarkSeen[3], landmarkSeen[4],
           millis() / 1000UL, WiFi.localIP().toString().c_str(), OTA_PORT, VERSION);
  mqtt.publish(MQTT_TOPIC_COMMAND, buf);
  mqttLogf("STATUS -> %s", buf);
}

// WatchTower 5-minute heartbeat, fleet-standard format, non-retained.
void heartBeat() {
  unsigned long now = millis();
  if (now - lastHeartbeat < HEARTBEAT_MS) return;
  lastHeartbeat = now;
  char buf[64];
  snprintf(buf, sizeof(buf), "HEARTBEAT:%s:UP%lus:RSSI%d",
           solved ? "SOLVED" : (crossingStarted ? "CROSSING" : "IDLE"),
           millis() / 1000UL, (int)WiFi.RSSI());
  mqtt.publish(MQTT_TOPIC_STATUS, buf);
}

// Returns 1..5 if topic is MermaidsTale/LandmarkN, else 0.
static uint8_t landmarkIndex(const char* topic) {
  for (uint8_t i = 0; i < LANDMARK_COUNT; i++)
    if (strcmp(topic, landmarkTopic[i]) == 0) return i + 1;
  return 0;
}

void handleCommand(char* msg) {
  // Our own replies come back on the same topic - don't treat them as commands.
  if (strcmp(msg, "OK") == 0 || strcmp(msg, "PONG") == 0 ||
      strncmp(msg, "ERR", 3) == 0 || strchr(msg, '|') != NULL) return;
  Serial.printf("[MQTT] command: %s\n", msg);
  if (strcmp(msg, "PING") == 0)   { mqtt.publish(MQTT_TOPIC_COMMAND, "PONG"); return; }
  if (strcmp(msg, "STATUS") == 0) { promptStatus(); return; }
  if (strcmp(msg, "RESET") == 0) {
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    Serial.println("[MQTT] RESET -> rebooting");
    delay(100);
    ESP.restart();
    return;
  }
  if (strcmp(msg, "CLEAR") == 0) {
    routeClear("CLEAR command");
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    return;
  }
  if (strcmp(msg, "LIGHTS_TEST") == 0) {
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    lightsSelfTest();
    return;
  }
  // PIXEL <n> - light ONLY LED n (1-based) white for 2 min, to find marker
  // positions on the strip. PIXEL OFF clears it.
  if (strncmp(msg, "PIXEL", 5) == 0) {
    const char* arg = msg + 5;
    while (*arg == ' ') arg++;
    walkActive = false;
    if (strcasecmp(arg, "OFF") == 0) { previewPixel = -1; }
    else {
      int n = atoi(arg);
      if (n < 1 || n > LED_COUNT) { mqtt.publish(MQTT_TOPIC_COMMAND, "ERR PIXEL 1-N|OFF"); return; }
      previewPixel    = n - 1;
      previewPixelEnd = millis() + PIXEL_HOLD_MS;
      mqttLogf("PIXEL %d lit (white) for 2 min", n);
    }
    lightsDirty = true;
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    return;
  }
  // WALK - one white LED steps from 1 to the end, one per second, so you
  // can count positions. WALK OFF stops it.
  if (strncmp(msg, "WALK", 4) == 0) {
    const char* arg = msg + 4;
    while (*arg == ' ') arg++;
    previewPixel = -1;
    if (strcasecmp(arg, "OFF") == 0) { walkActive = false; }
    else {
      walkActive = true;
      walkIndex  = 0;
      walkNextMs = millis() + LED_STEP_MS;
      mqttLogf("WALK started at LED 1, one per second");
    }
    lightsDirty = true;
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    return;
  }
  // PREVIEW <0-5> | NEXT | FULL | OFF - bench aid. Drives the SAME route
  // logic the game does, so you see the real 1-pixel-per-second draw,
  // without publishing to any game topic.
  //   PREVIEW 0    = intro only (segment 1)
  //   PREVIEW 3    = intro + Landmark1..3 (segments 1-4)
  //   PREVIEW NEXT = fire the next cue in order (intro, then L1, L2 ...)
  //   PREVIEW FULL = whole crossing, SOLVED
  //   PREVIEW OFF  = dark
  if (strncmp(msg, "PREVIEW", 7) == 0) {
    const char* arg = msg + 7;
    while (*arg == ' ') arg++;
    if (strcasecmp(arg, "OFF") == 0) {
      routeClear("PREVIEW OFF");
    } else if (strcasecmp(arg, "FULL") == 0) {
      routeClear("PREVIEW FULL");
      onIntro();
      for (uint8_t i = 1; i <= LANDMARK_COUNT; i++) onLandmark(i);
    } else if (strcasecmp(arg, "NEXT") == 0) {
      if (!crossingStarted) {
        onIntro();
      } else {
        uint8_t n = 1;
        while (n <= LANDMARK_COUNT && landmarkSeen[n - 1]) n++;
        if (n <= LANDMARK_COUNT) onLandmark(n);
      }
    } else if (*arg >= '0' && *arg <= '9') {
      int n = atoi(arg);
      if (n > LANDMARK_COUNT) { mqtt.publish(MQTT_TOPIC_COMMAND, "ERR range 0-5"); return; }
      routeClear("PREVIEW");
      onIntro();
      for (int i = 1; i <= n; i++) onLandmark((uint8_t)i);
    } else {
      mqtt.publish(MQTT_TOPIC_COMMAND, "ERR PREVIEW <0-5|NEXT|FULL|OFF>");
      return;
    }
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    return;
  }
  Serial.printf("[MQTT] unknown command: %s\n", msg);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[128];
  if (length >= sizeof(message)) length = sizeof(message) - 1;
  memcpy(message, payload, length);
  message[length] = '\0';

  char* msg = message;
  while (*msg == ' ' || *msg == '\r' || *msg == '\n') msg++;
  char* end = msg + strlen(msg) - 1;
  while (end > msg && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';

  if (strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
    if (*msg == '\0') return;   // retained-erase publishes "" - not a command
    handleCommand(msg);
    return;
  }

  // ---- Game topics below. Empty payload = the retained-erase sweeper.
  if (*msg == '\0') return;
  // Anything that lands right after subscribing is a retained replay from
  // before we booted, not a live event - never light the strip off it.
  if (millis() - subscribedAtMs < SETTLE_MS) {
    Serial.printf("[MQTT] settle: ignoring %s = %s\n", topic, msg);
    return;
  }

  if (strcmp(topic, TOPIC_GAME_RESET) == 0) { routeClear("GameReset"); return; }
  if (strcmp(topic, TOPIC_GAME_START) == 0) { routeClear("GameStart"); return; }

  if (strcmp(topic, TOPIC_INTRO) == 0) {
    if (strcmp(msg, "trigger") == 0) onIntro();      // exact word from Unreal
    else Serial.printf("[MQTT] ObstacleCourseIntro = %s (ack, ignored)\n", msg);
    return;
  }

  uint8_t n = landmarkIndex(topic);
  if (n) {
    if (isTruthy(msg)) onLandmark(n);
    else Serial.printf("[MQTT] Landmark%u = %s (not truthy, ignored)\n", (unsigned)n, msg);
    return;
  }
}

//================================================
//            Setup / loop
//================================================
void setupLights() {
  FastLED.addLeds<WS2812B, LED_DATA_PIN, LED_COLOR_ORDER>(leds, LED_COUNT);
  FastLED.setBrightness(LED_BRIGHTNESS);
  fill_solid(leds, LED_COUNT, COLOR_OFF);
  FastLED.show();
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Native-USB serial BLOCKS every print while no PC is reading the port
  // (bench 2026-09-22: pixels lit 1 per ~5 s instead of 1 per 1 s until a
  // terminal was opened). Zero timeout = drop the bytes, never stall loop().
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.printf("\n%s v%s\n", PROP_NAME, VERSION);

#if !defined(ESP8266)
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdtCfg = {};
    wdtCfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
    wdtCfg.idle_core_mask = 0;
    wdtCfg.trigger_panic  = true;
    esp_task_wdt_reconfigure(&wdtCfg);
  #else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
  #endif
  esp_task_wdt_add(NULL);
#endif

  for (uint8_t i = 0; i < LANDMARK_COUNT; i++)
    snprintf(landmarkTopic[i], TOPIC_BUF, "%s%u", TOPIC_LANDMARK_PREFIX, (unsigned)(i + 1));

  setupLights();
  lightsSelfTest();   // R/G/B sweep the moment power lands - proves wiring

  ensureWiFi();
  setupOTA();          // mandatory: wireless re-flash listener, right after Wi-Fi
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(256);
  ensureMqtt();
  lastMqttOkMs = millis();
}

void loop() {
  wdtFeed();
  ensureWiFi();
  ArduinoOTA.handle();  // mandatory: service OTA every loop
  ensureMqtt();
  mqtt.loop();

  if (mqtt.connected()) {
    lastMqttOkMs = millis();
  } else if (millis() - lastMqttOkMs >= OFFLINE_REBOOT_MS) {
    Serial.println("[WDT] no broker for 2 min - restarting");
    ESP.restart();
  }

  serviceAnimation();               // one pixel per second toward the goal
  serviceBenchHelpers();            // PIXEL / WALK bench helpers
  if (lightsDirty) renderLights();  // one show() per change, never per loop
  heartBeat();
}
