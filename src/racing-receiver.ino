#include <ESP8266WiFi.h>
#include <time.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <string.h>

#define USE_CLIENTSSL true
#define BEEPER_PIN 14
#define LASER_PIN 12
#define DEVICE_ID "GATE-1"
#define BROADCAST_PORT 8266
#define UNICAST_PORT 8267
#define MQTT_GATE_STATUS_TOPIC "gates/status"
#define MQTT_GATE_SWITCH_TOPIC "gates/switch"

const char *ssid = "nd0ut";
const char *pass = "pidorpidor";

WiFiUDP udp;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool gateOpen = false;

enum BroadcastActionType
{
  DISCOVER = 0
};

enum Status
{
  IDLE,
  WAIT_WIFI,
  WAIT_NTP,
  WAIT_MQTT_SERVER_INFO,
  WAIT_MQTT_SERVER_CONNECT,
  CONNECTED,
  FAILED,
};

int status = Status::IDLE;

void setStatus(Status newStatus)
{
  status = newStatus;
  Serial.print("Status changed to ");
  Serial.println(status);
}

int64_t xx_time_get_time()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

char *jsonTimestamp()
{
  int64_t time = xx_time_get_time();
  static char time_str[32];
  time_t t = time / 1000;
  strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", localtime(&t));
  sprintf(time_str + strlen(time_str), ".%03dZ", (int)(time % 1000));
  return time_str;
}

void beep(int count = 1, int delayMs = 100)
{
  for (int i = 0; i < count; i++)
  {
    digitalWrite(BEEPER_PIN, HIGH);
    delay(100);
    digitalWrite(BEEPER_PIN, LOW);
    delay(delayMs);
  }
}

void sendBroadcastMessage(BroadcastActionType actionType)
{

  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["action"] = actionType;
  doc["unicastPort"] = UNICAST_PORT;

  String string;
  serializeJson(doc, string);

  char msg[255];
  string.toCharArray(msg, 255);

  udp.beginPacket("255.255.255.255", BROADCAST_PORT);
  udp.write(msg);
  udp.endPacket();
}

JsonDocument receiveServerInfo()
{
  int packetSize = udp.parsePacket();
  JsonDocument doc;

  if (packetSize)
  {
    char packetBuffer[255];
    int len = udp.read(packetBuffer, 255);
    if (len > 0)
    {
      packetBuffer[len] = 0;
    }

    DeserializationError result = deserializeJson(doc, packetBuffer);
    if (result != DeserializationError::Ok)
    {
      Serial.print("Failed to parse JSON");
    }

    Serial.println("Received server info: ");
    serializeJson(doc, Serial);
    Serial.println("");
  }

  return doc;
}

String getConnectionStatusMessage(String status)
{
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["connectionStatus"] = status;
  String string;
  serializeJson(doc, string);
  return string;
}

String getCircuitStatusMessage(String status)
{
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["circuitStatus"] = status;
  doc["timestamp"] = jsonTimestamp();
  String string;
  serializeJson(doc, string);
  return string;
}

bool connectMqttServer(JsonDocument serverInfo)
{
  String mqttServer = serverInfo["mqttServer"];
  String mqttPort = serverInfo["mqttPort"];
  String mqttUsername = serverInfo["mqttUsername"];
  String mqttPassword = serverInfo["mqttPassword"];

  Serial.println("Connecting to MQTT server");
  Serial.print("Server: ");
  Serial.println(mqttServer);
  Serial.print("Port: ");
  Serial.println(mqttPort);
  Serial.print("Username: ");
  Serial.println(mqttUsername);
  Serial.print("Password: ");
  Serial.println(mqttPassword);

  mqttClient.setServer(mqttServer.c_str(), mqttPort.toInt());
  bool connected = mqttClient.connect(DEVICE_ID, mqttUsername.c_str(), mqttPassword.c_str(), MQTT_GATE_STATUS_TOPIC, 0, false, getConnectionStatusMessage("offline").c_str(), false);
  if (!connected)
  {
    Serial.println("Failed to connect to MQTT server");
    return false;
  }

  mqttClient.publish(MQTT_GATE_STATUS_TOPIC, getConnectionStatusMessage("online").c_str(), true);
  Serial.println("Connected to MQTT server");
  return true;
}

void checkGate()
{
  int laserState = digitalRead(LASER_PIN);

  if (laserState == HIGH)
  {
    digitalWrite(BEEPER_PIN, HIGH);
  }
  else
  {
    digitalWrite(BEEPER_PIN, LOW);
  }

  if (laserState == HIGH && !gateOpen)
  {
    gateOpen = true;
    mqttClient.publish(MQTT_GATE_SWITCH_TOPIC, getCircuitStatusMessage("opened").c_str());
  }
  else if (laserState == LOW && gateOpen)
  {
    gateOpen = false;
    mqttClient.publish(MQTT_GATE_SWITCH_TOPIC, getCircuitStatusMessage("closed").c_str());
  }
}

void setupNtp()
{
  Serial.println("Retrieving time...");
  setStatus(Status::WAIT_NTP);
  configTime("time.google.com", "time.windows.com", "pool.ntp.org");
  while (time(NULL) < 1000000000)
  {
    delay(1000);
  }

  char *time = jsonTimestamp();
  Serial.println(time);
}

void setupBroadcast()
{
  setStatus(Status::WAIT_MQTT_SERVER_INFO);
  udp.begin(UNICAST_PORT);
}

void setupWifi()
{
  setStatus(Status::WAIT_WIFI);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print("Connecting..");
  }

  Serial.println("success!");
  Serial.print("IP Address is: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BEEPER_PIN, OUTPUT);
  pinMode(LASER_PIN, INPUT);

  beep(1);

  Serial.begin(115200);

  setupWifi();
  setupNtp();
  setupBroadcast();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED && status != Status::IDLE)
  {
    Serial.println("WiFi connection lost");
    setupWifi();
    setupBroadcast();
  }

  if (status == Status::WAIT_MQTT_SERVER_INFO)
  {
    sendBroadcastMessage(BroadcastActionType::DISCOVER);
    delay(1000);
    Serial.println("Receiving server info");

    JsonDocument serverInfo = receiveServerInfo();
    if (serverInfo.isNull())
    {
      return;
    }
    setStatus(Status::WAIT_MQTT_SERVER_CONNECT);
    bool connected = connectMqttServer(serverInfo);
    if (!connected)
    {
      setStatus(Status::FAILED);
      Serial.println("Failed to connect to MQTT server");
      return;
    }
    setStatus(Status::CONNECTED);
  }

  if (status == Status::CONNECTED)
  {
    bool connected = mqttClient.loop();
    if (!connected)
    {
      Serial.println("MQTT connection lost");
      setupBroadcast();
    }
  }

  if (status == Status::CONNECTED)
  {
    checkGate();
  }
}