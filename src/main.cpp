#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <ArduinoOTA.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <IotWebConfTParameter.h>
#include <IotWebConfESP32HTTPUpdateServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <uptime.h>
#include <algorithm>
#include <esp_core_dump.h>

#define STRING_LEN 128
#define nils_length(x) ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))
// #define nils_length( x ) ( sizeof(x) )

const int IMPULSEPIN = GPIO_NUM_27;

unsigned long timeDetected = 0;
unsigned long timeReleased = 0;
unsigned int impulseCounted = 0;
unsigned int impulsePinChanged = 0;
unsigned int mqttImpulseCounted = 0;
unsigned int nvsImpulseCounted = 0;
bool impulsePinState = false;
bool needReset = false;
byte heartbeatError = 0;
byte mqttHeartbeatError = 0;
char impulseUnit[STRING_LEN];

// For a cloud MQTT broker, type the domain name
// #define MQTT_HOST "example.com"
#define MQTT_PORT 1883
#define MQTT_PUB_IMPULSE_OUT1 "imp_counted_1"
#define MQTT_PUB_VALUE_OUT1 "imp_value_1"
#define MQTT_PUB_HEARTBEAT "heartbeat"
#define MQTT_PUB_DOWNTIME "downtime"
#define MQTT_PUB_INFO "info"
#define MQTT_PUB_STATUS "status"
AsyncMqttClient mqttClient;
String mqttDisconnectReason;
char mqttDisconnectTime[40];
char mqttServer[STRING_LEN];
char mqttUser[STRING_LEN];
char mqttPassword[STRING_LEN];
char mqttTopicPath[STRING_LEN];

Ticker mqttReconnectTimer;
Ticker secTimer;
Ticker sec10Timer;
Ticker min10Timer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
char ntpServer[STRING_LEN];
char ntpTimezone[STRING_LEN];
time_t now;
struct tm localTime;

char hostname[STRING_LEN];
#define CONFIG_VERSION "2"
Preferences preferences;
int iotWebConfPinState = HIGH;
unsigned long iotWebConfPinChanged = 0;
DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
char impulseCountedStr[10];
IotWebConf iotWebConf("Gaszaehler", &dnsServer, &server, "", CONFIG_VERSION);
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("server", "mqttServer", mqttServer, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("user", "mqttUser", mqttUser, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("password", "mqttPassword", mqttPassword, STRING_LEN);
IotWebConfTextParameter mqttTopicPathParam = IotWebConfTextParameter("topicpath", "mqttTopicPath", mqttTopicPath, STRING_LEN, "ht/gas/");
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP");
IotWebConfTextParameter ntpServerParam = IotWebConfTextParameter("server", "ntpServer", ntpServer, STRING_LEN, "de.pool.ntp.org");
IotWebConfTextParameter ntpTimezoneParam = IotWebConfTextParameter("timezone", "ntpTimezone", ntpTimezone, STRING_LEN, "CET-1CEST,M3.5.0/02,M10.5.0/03");
IotWebConfParameterGroup impulseGroup = IotWebConfParameterGroup("impulse", "Impulse");
IotWebConfTextParameter impulseUnitParam = IotWebConfTextParameter("unit", "impulseUnit", impulseUnit, STRING_LEN, "m3");
iotwebconf::FloatTParameter impulseMultiplierParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("impulseMultiplierParam").label("multiplier").defaultValue(1.0).step(0.01).placeholder("e.g. 23.4").build();
IotWebConfNumberParameter impulseCountedParam = IotWebConfNumberParameter("impulses counted", "impulseCounted", impulseCountedStr, 10, "0");

// -- SECTION: Common functions
int mod(int x, int y)
{
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}

// Necessary forward declarations
String getStatus();
String getStatusJson();
void setTimezone(String timezone);
void connectToMqtt();
void mqttSendTopics(bool mqttInit = false);
//--

void saveImpulseToNvs()
{
  if (nvsImpulseCounted != impulseCounted)
  {
    nvsImpulseCounted = impulseCounted;
    preferences.putUInt("impulseCounter", nvsImpulseCounted);
  }
}

// -- SECTION: Wifi Manager
String verbose_print_reset_reason(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_UNKNOWN:
    return (" Reset reason can not be determined");
  case ESP_RST_POWERON:
    return ("Reset due to power-on event");
  case ESP_RST_EXT:
    return ("Reset by external pin (not applicable for ESP32)");
  case ESP_RST_SW:
    return ("Software reset via esp_restart");
  case ESP_RST_PANIC:
    return ("Software reset due to exception/panic");
  case ESP_RST_INT_WDT:
    return ("Reset (software or hardware) due to interrupt watchdog");
  case ESP_RST_TASK_WDT:
    return ("Reset due to task watchdog");
  case ESP_RST_WDT:
    return ("Reset due to other watchdogs");
  case ESP_RST_DEEPSLEEP:
    return ("Reset after exiting deep sleep mode");
  case ESP_RST_BROWNOUT:
    return ("Brownout reset (software or hardware)");
  case ESP_RST_SDIO:
    return ("Reset over SDIO");
  default:
    return ("NO_MEAN");
  }
}

bool checkCoreDump()
{
  size_t size = 0;
  size_t address = 0;
  if (esp_core_dump_image_get(&address, &size) == ESP_OK)
  {
    const esp_partition_t *pt = NULL;
    pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
    if (pt != NULL)
      return true;
    else
      return false;
  }
  else
    return false;
}

String readCoreDump()
{
  size_t size = 0;
  size_t address = 0;
  if (esp_core_dump_image_get(&address, &size) == ESP_OK)
  {
    const esp_partition_t *pt = NULL;
    pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");

    if (pt != NULL)
    {
      uint8_t bf[256];
      char str_dst[640];
      int16_t toRead;
      String return_str;

      for (int16_t i = 0; i < (size / 256) + 1; i++)
      {
        strcpy(str_dst, "");
        toRead = (size - i * 256) > 256 ? 256 : (size - i * 256);

        esp_err_t er = esp_partition_read(pt, i * 256, bf, toRead);
        if (er != ESP_OK)
        {
          Serial.printf("FAIL [%x]\n", er);
          break;
        }

        for (int16_t j = 0; j < 256; j++)
        {
          char str_tmp[3];

          sprintf(str_tmp, "%02x", bf[j]);
          strcat(str_dst, str_tmp);
        }

        return_str += str_dst;
      }
      return return_str;
    }
    else
    {
      return "Partition NULL";
    }
  }
  else
  {
    return "esp_core_dump_image_get() FAIL";
  }
}

void crash_me_hard()
{
  // provoke crash through writing to a nullpointer
  volatile uint32_t *aPtr = (uint32_t *)0x00000000;
  *aPtr = 0x1234567; // goodnight
}

void startCrashTimer(int secs)
{
  for (int i = 0; i <= secs; i++)
  {
    printf("Crashing in %d seconds..\n", secs - i);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  printf("Crashing..\n");
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  crash_me_hard();
}

void startCrash()
{
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "Crashing in 5 seconds...!";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
  startCrashTimer(5);
}

void handleCoreDump()
{
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=coredump.bin");
  server.sendHeader("Connection", "close");
  server.send(200, "application/octet-stream", readCoreDump());
}

void handleDeleteCoreDump()
{
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Warmwater Recirculation Pump</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  if (esp_core_dump_image_erase() == ESP_OK)
  {
    s += "Core dump deleted";
    s += "<button type=\"button\" onclick=\"javascript:history.back()\">Back</button>";
  }
  else
    s += "No core dump found!";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}

// -- SECTION: Wifi Manager
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  char tempStr[128];

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Impulsemeter</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "<fieldset id=" + String(mqttGroup.getId()) + ">";
  s += "<legend>" + String(mqttGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(mqttServerParam.label) + ": </td>";
  s += "<td>" + String(mqttServer) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(mqttTopicPathParam.label) + ": </td>";
  s += "<td>" + String(mqttTopicPath) + "</td>";
  s += "</tr><tr>";
  s += "<td>last disconnect reason: </td>";
  s += "<td>" + mqttDisconnectReason + "</td>";
  s += "</tr><tr>";
  s += "<td>last disconnect: </td>";
  s += "<td>" + String(mqttDisconnectTime) + "</td>";
  s += "</tr><tr>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=" + String(ntpGroup.getId()) + ">";
  s += "<legend>" + String(ntpGroup.label) + "</legend>";
  s += "<table border = \"0\"><tr>";
  s += "<td>" + String(ntpServerParam.label) + ": </td>";
  s += "<td>" + String(ntpServer) + "</td>";
  s += "</tr><tr>";
  s += "<td>" + String(ntpTimezoneParam.label) + ": </td>";
  s += "<td>" + String(ntpTimezone) + "</td>";
  s += "</tr><tr>";
  s += "<td>actual local time: </td>";
  strftime(tempStr, 40, "%d.%m.%Y %T", &localTime);
  s += "<td>" + String(tempStr) + "</td>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=\"status\">";
  s += "<legend>Status</legend>";
  s += "<p>impulse counter: ";
  s += impulseCounted;
  s += "<p>consumption: ";
  s += float(impulseCounted) * impulseMultiplierParam.value();
  s += impulseUnit;
  uptime::calculateUptime();
  sprintf(tempStr, "%04u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  s += "<p>uptime: " + String(tempStr);
  s += "<p>last reset reason: " + verbose_print_reset_reason(esp_reset_reason());
  s += "<p>heartbeat: ";
  switch (heartbeatError)
  {
  case 0:
    s += "unchecked";
    break;
  case 1:
    s += "ok";
    break;
  case 2:
    s += "downtime too long";
    break;
  }
  s += "<p>";
  switch (esp_core_dump_image_check())
  {
  case ESP_OK:
    s += "<a href=/coredump>core dump found</a> - <a href=/deletecoredump>delete core dump</a>";
    break;
  case ESP_ERR_NOT_FOUND:
    s += "no core dump found";
    break;
  case ESP_ERR_INVALID_SIZE:
    s += "core dump with invalid size - <a href=/deletecoredump>delete core dump</a>";
    break;
  case ESP_ERR_INVALID_CRC:
    s += "core dump with invalid CRC - <a href=/deletecoredump>delete core dump</a>";
  }
  s += "</fieldset>";

  s += "<p>Go to <a href='config'>Configuration</a>";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}

void configSaved()
{
  // check if Wifi configration has changed - if yes, restart
  if (preferences.isKey("apPassword"))
  {
    if (strcmp(iotWebConf.getApPasswordParameter()->valueBuffer, preferences.getString("apPassword").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;
  if (preferences.isKey("wifiSsid"))
  {
    if (strcmp(iotWebConf.getWifiSsidParameter()->valueBuffer, preferences.getString("wifiSsid").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;

  if (preferences.isKey("wifiPassword"))
  {
    if (strcmp(iotWebConf.getWifiPasswordParameter()->valueBuffer, preferences.getString("wifiPassword").c_str()) != 0)
      needReset = true;
  }
  else
    needReset = true;

  if (iotWebConf.getApPasswordParameter()->getLength() > 0)
    preferences.putString("apPassword", String(iotWebConf.getApPasswordParameter()->valueBuffer));
  preferences.putString("wifiSsid", String(iotWebConf.getWifiAuthInfo().ssid));
  preferences.putString("wifiPassword", String(iotWebConf.getWifiAuthInfo().password));

  impulseCounted = atol(impulseCountedStr);
  saveImpulseToNvs();
  if (timeClient.isTimeSet())
  {
    heartbeatError = 1;
    preferences.putULong("heartbeat", timeClient.getEpochTime());
  }

  // restart MQTT connection
  mqttClient.disconnect();
  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);
  Serial.println("MQTT ready");
  connectToMqtt();

  // restart NTP connection
  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);
  Serial.println("NTP ready");

  Serial.println("Configuration saved.");
}

bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper)
{
  Serial.println("Validating form.");
  bool valid = true;

  // int l = webRequestWrapper->arg(mqttServerParam.getId()).length();
  // if (l < 3)
  // {
  //   mqttServerParam.errorMessage = "Please enter at least 3 chars!";
  //   valid = false;
  // }

  return valid;
}

//-- SECTION: NTP
void setTimezone(String timezone)
{
  Serial.printf("  Setting Timezone to %s\n", ntpTimezone);
  setenv("TZ", ntpTimezone, 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}

void getLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time 1");
    return;
  }
  localTime = timeinfo;
}

void updateTime()
{
  if (iotWebConf.getState() == 4)
  {
    timeClient.update();
    getLocalTime();
  }
}

//-- SECTION: connection handling
void connectToMqtt()
{
  if (strlen(mqttServer) > 0)
  {
    Serial.println("Connecting to MQTT...");
    mqttClient.connect();
  }
}

void onWifiConnected()
{
  Serial.println("Connected to Wi-Fi.");
  Serial.println(WiFi.localIP());
  connectToMqtt();
  ArduinoOTA.begin();
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  // ArduinoOTA.end();
}

void onMqttConnect(bool sessionPresent)
{
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  uint16_t packetIdSub;
  mqttSendTopics(true);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  switch (reason)
  {
  case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
    mqttDisconnectReason = "TCP_DISCONNECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
    mqttDisconnectReason = "MQTT_UNACCEPTABLE_PROTOCOL_VERSION";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
    mqttDisconnectReason = "MQTT_IDENTIFIER_REJECTED";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
    mqttDisconnectReason = "MQTT_SERVER_UNAVAILABLE";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
    mqttDisconnectReason = "MQTT_MALFORMED_CREDENTIALS";
    break;
  case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
    mqttDisconnectReason = "MQTT_NOT_AUTHORIZED";
    break;
  }
  strftime(mqttDisconnectTime, 40, "%d.%m.%Y %T", &localTime);

  Serial.printf(" [%8u] Disconnected from the broker reason = %s\n", millis(), mqttDisconnectReason.c_str());
  if (WiFi.isConnected())
  {
    Serial.printf(" [%8u] Reconnecting to MQTT..\n", millis());
    mqttReconnectTimer.once(5, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
  Serial.printf(" [%8u] Subscribe acknowledged id: %u, qos: %u\n", millis(), packetId, qos);
}

void onMqttPublish(uint16_t packetId)
{
  // Serial.print("Publish acknowledged.");
  // Serial.print("  packetId: ");
  // Serial.println(packetId);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
  // Serial.println("Publish received.");
  // Serial.print("  topic: ");
  // Serial.println(topic);
  // Serial.print("  qos: ");
  // Serial.println(properties.qos);
  // Serial.print("  payload: ");
  // Serial.println(payload);
  // Serial.print("  dup: ");
  // Serial.println(properties.dup);
  // Serial.print("  retain: ");
  // Serial.println(properties.retain);
  // Serial.print("  len: ");
  // Serial.println(len);
  // Serial.print("  index: ");
  // Serial.println(index);
  // Serial.print("  total: ");
  // Serial.println(total);
}

void mqttPublish(const char *topic, const char *payload)
{
  std::string tempTopic;
  tempTopic.append(mqttTopicPath);
  tempTopic.append(topic);
  mqttClient.publish(tempTopic.c_str(), 0, true, payload);
}
//-- END SECTION: connection handling

void mqttPublishUptime()
{
  char msg_out[20];
  uptime::calculateUptime();
  sprintf(msg_out, "%04u %s %02u:%02u:%02u", uptime::getDays(), "days", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  // Serial.println(msg_out);
  mqttPublish(MQTT_PUB_INFO, msg_out);
}

void mqttSendTopics(bool mqttInit)
{
  char msg_out[20];
  if (impulseCounted != mqttImpulseCounted || mqttInit)
  {
    sprintf(msg_out, "%d", impulseCounted);
    mqttImpulseCounted = impulseCounted;
    mqttPublish(MQTT_PUB_IMPULSE_OUT1, msg_out);
    float value = float(impulseCounted) * impulseMultiplierParam.value();
    sprintf(msg_out, "%.2f", value);
    mqttPublish(MQTT_PUB_VALUE_OUT1, msg_out);
  }

  if (heartbeatError != mqttHeartbeatError || mqttInit)
  {
    switch (heartbeatError)
    {
    case 0:
      strcpy(msg_out, "unchecked");
      break;
    case 1:
      strcpy(msg_out, "ok");
      break;
    case 2:
      strcpy(msg_out, "downtime too long");
      break;
    }
    mqttHeartbeatError = heartbeatError;
    mqttPublish(MQTT_PUB_HEARTBEAT, msg_out);
  }

  if (mqttInit)
    mqttPublishUptime();
}

void onSec10Timer()
{
  // check heartbeat and set errorstate - check onetime if NTP is available the first time
  if (timeClient.isTimeSet() && heartbeatError == 0 && preferences.isKey("heartbeat"))
  {
    Serial.print("heartbeat: ");
    Serial.println(preferences.getULong("heartbeat"));
    if (timeClient.getEpochTime() - preferences.getULong("heartbeat") > (600 + (millis() / 1000))) // 10 minutes offline leads into an error
      heartbeatError = 2;
    else
      heartbeatError = 1;
    char msg_out[20];
    sprintf(msg_out, "%d", timeClient.getEpochTime() - preferences.getULong("heartbeat") - (millis() / 1000));
    Serial.print("difference: ");
    Serial.println(msg_out);
    mqttPublish(MQTT_PUB_DOWNTIME, msg_out);
  }

  mqttSendTopics();
}

void onMin5Timer()
{
  mqttPublishUptime();
  saveImpulseToNvs();

  // heartbeat - save NTP time in NVS
  if (timeClient.isTimeSet() && (heartbeatError == 1 || !preferences.isKey("heartbeat")))
  {
    Serial.print("heartbeat saved: ");
    Serial.println(timeClient.getEpochTime());
    preferences.putULong("heartbeat", timeClient.getEpochTime());
  }
}

void setup()
{
  // basic setup
  Serial.begin(115200);
  esp_core_dump_init();
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(IMPULSEPIN, INPUT_PULLUP);
  impulsePinState = digitalRead(IMPULSEPIN); // init PIN state
  digitalWrite(LED_BUILTIN, LOW);

  // WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if (!preferences.begin("wifi"))
  {
    Serial.println("Error opening NVS-Namespace");
    for (;;)
      ; // leere Dauerschleife -> Ende
  }
  iotWebConf.setupUpdateServer(
      [](const char *updatePath)
      { httpUpdater.setup(&server, updatePath); },
      [](const char *userName, char *password)
      { httpUpdater.updateCredentials(userName, password); });

  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  mqttGroup.addItem(&mqttTopicPathParam);
  iotWebConf.addParameterGroup(&mqttGroup);
  ntpGroup.addItem(&ntpServerParam);
  ntpGroup.addItem(&ntpTimezoneParam);
  iotWebConf.addParameterGroup(&ntpGroup);
  impulseGroup.addItem(&impulseUnitParam);
  impulseGroup.addItem(&impulseMultiplierParam);
  impulseGroup.addItem(&impulseCountedParam);
  iotWebConf.addParameterGroup(&impulseGroup);

  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&onWifiConnected);

  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    Serial.println("Invalid config detected - restoring WiFi settings...");
    // much better handling than iotWebConf library to avoid lost wifi on configuration change
    if (preferences.isKey("apPassword"))
      strncpy(iotWebConf.getApPasswordParameter()->valueBuffer, preferences.getString("apPassword").c_str(), iotWebConf.getApPasswordParameter()->getLength());
    else
      Serial.println("AP Password not found for restauration.");
    if (preferences.isKey("wifiSsid"))
      strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer, preferences.getString("wifiSsid").c_str(), iotWebConf.getWifiSsidParameter()->getLength());
    else
      Serial.println("WiFi SSID not found for restauration.");
    if (preferences.isKey("wifiPassword"))
      strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, preferences.getString("wifiPassword").c_str(), iotWebConf.getWifiPasswordParameter()->getLength());
    else
      Serial.println("WiFi Password not found for restauration.");
    iotWebConf.saveConfig();
    iotWebConf.resetWifiAuthInfo();
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []
            {
              itoa(impulseCounted, impulseCountedStr, 10);
              iotWebConf.handleConfig(); });
  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });
  server.on("/coredump", handleCoreDump);
  server.on("/deletecoredump", handleDeleteCoreDump);
  server.on("/crash", startCrash);
  Serial.println("Wifi manager ready.");

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onSubscribe(onMqttSubscribe);

  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, MQTT_PORT);
  Serial.println("MQTT ready");

  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);
  Serial.println("NTP ready");

  // Init OTA function
  ArduinoOTA.onStart([]()
                     { Serial.println("Start OTA"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("OTA Progress: %u%%\n\r", (progress / (total / 100))); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nEnd OTA"); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
  Serial.println("OTA Ready");

  impulseCounted = preferences.getUInt("impulseCounter", 0);
  nvsImpulseCounted = impulseCounted;

  // Timers
  sec10Timer.attach(10, onSec10Timer);
  min10Timer.attach(300, onMin5Timer);
}

void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  updateTime();

  if (needReset)
  {
    Serial.println("Rebooting in 1 second.");
    preferences.putUInt("impulseCounter", impulseCounted);
    iotWebConf.delay(1000);
    ESP.restart();
  }

  // Check impulse
  unsigned long now = millis();
  if ((80 < now - impulsePinChanged) && (impulsePinState != digitalRead(IMPULSEPIN)))
  {
    impulsePinState = 1 - impulsePinState; // invert pin state as it is changed
    impulsePinChanged = now;
    digitalWrite(LED_BUILTIN, HIGH);

    if (impulsePinState) // button pressed action - set pressed time
    {
      // button released
      timeReleased = millis();
      Serial.println("Impulse released");
      Serial.print("Impulse State: ");
      Serial.print(impulsePinState);
      Serial.print(", Time: ");
      Serial.println(timeReleased - timeDetected);

      char msg_out[40];
      sprintf(msg_out, "Impulse released: %06u", timeReleased - timeDetected);
      mqttPublish(MQTT_PUB_INFO, msg_out);
      impulseCounted++;
      digitalWrite(LED_BUILTIN, LOW);
    }
    else
    {
      timeDetected = now;
      Serial.println("Impulse detected");
      mqttPublish(MQTT_PUB_INFO, "Impulse detected");
    }
  }
}