#include <FastLED.h>
#include <PubSubClient.h>
#include <Ethernet.h>

#define TOTAL_LEDS 15
#define LEDS_DATA_PIN A0
#define LED_TYPE WS2812B


//************ GLOBAL VARIABLES **********
WiFiClient espClient;
PubSubClient mqtt(espClient);

const char * ssid = "AlchemyGuest";
const char * password = "VoodooVacation5601";
const char * mqttServer = "10.1.10.115";

unsigned char position = 0;                                             //position of the ship, start = 0, finish = TOTAL_LEDS
unsigned char rgb[3] = {255,0,0};                                       //an array to hold the rgb values of the default LED color. 

CRGB leds[TOTAL_LEDS];                                                  //creation of array RGB structure to hold each LED value

//************* FUNCTIONS ***************
//MQTT & NETWORK
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to Wi-Fi");
    Serial.print("WiFi.status() = ");
    Serial.println(WiFi.status());
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP8266Client_01")) {
      Serial.println("connected");
      client.publish("MermaidsTale/ShipMap", "Connected!");
      client.subscribe("MermaidsTale/#");

      // Initial publish now that we're definitely connected
      client.publish("MermaidsTale/ShipMap", "Searching");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(". Trying again in 5 seconds.");
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++)
    message += (char)payload[i];
  Serial.print("MQTT received: ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  //parse the data received from the mqtt server to pass into function
  if (strcmp(topic, "") == 0) {
    //if the information is different than the last, update the position 
    updateMap();
  }
}

void publishStatus() {
  if (!mqtt.connected())
    return;
  String summary = "";
  mqtt.publish("ShipMap/summary", summary.c_str());
}

void serverInit(){
  setup_wifi();
  mqtt.setServer(mqttServer, 1883);
  mqtt.setCallback(mqttCallback);
}

//GENERAL FUNCTIONS
void mapPosition(unsigned char data){
  //mapping of the data from the mqtt and led position
}
void updateMap() {
  //do the necessary mapping from the mqtt data and up the LED strip
  leds[position] = CRGB(rgb[0],rgb[1],rgb[2]);
  FastLED.show();
  mqtt.publish("ShipMap", "Position: ");
}

void led_init() {
  FastLED.addLeds<LED_TYPE, LEDS_DATA_PIN, GRB>(leds, TOTAL_LEDS);
  leds[0] = CGRB(rgb[0],rgb[1],rgb[2]);
  FastLED.show();
}

void _init(){
  led_init();
}

void program(){
  if(mqtt.connected()){
    reconnect();
  }
  mqtt.loop();
}

//*********** SETUP **********
void setup(){
  _init();
}
//*********** LOOP **********
void loop(){
  program();
}
