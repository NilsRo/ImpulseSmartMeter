#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Ticker.h>
#include <arduino-timer.h>
#include <AsyncMqttClient.h>
#include <map>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
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
#include <nvs.h>

#define STRING_LEN 128
#define DNS_LEN 254
#define HOSTNAME_LEN 64
#define MQTT_LEN 256
#define nils_length(x) ((sizeof(x) / sizeof(0 [x])) / ((size_t)(!(sizeof(x) % sizeof(0 [x])))))
// #define nils_length( x ) ( sizeof(x) )

const unsigned int MAX_DOWNTIME = 600;
const unsigned int DEBOUNCE_DELAY = 200; // Entprelldauer (in Millisekunden)

unsigned long timeDetected = 0;
unsigned long timeReleased = 0;
unsigned int impulseCounted = 0;
unsigned int mqttImpulseCounted = 0;
unsigned int nvsImpulseCounted = 0;
unsigned int impulsePin = GPIO_NUM_27;
unsigned int impulseLed = GPIO_NUM_2;
char impulsePinStr[3];
char impulseLedStr[3];
bool needReset = false;
char impulseMeterId[STRING_LEN];
char impulseMeterName[STRING_LEN];
byte heartbeatError = 1;
byte mqttHeartbeatError = 0;
char impulseUnit[STRING_LEN];
long downtime = 0;
JsonDocument historicalData;
bool nvsStatus = false;
auto timer = timer_create_default();

#define MQTT_SUB_CMND_IMPULSE "command/set_impulse"
#define MQTT_PUB_ACT "meters"
#define MQTT_PUB_HIST "historic"
#define MQTT_PUB_INFO "status/info"
#define MQTT_PUB_SYSINFO "status/sysinfo"
#define MQTT_PUB_STATUS "status/status"
#define MQTT_PUB_WIFI "status/wifi"
AsyncMqttClient mqttClient;
String mqttDisconnectReason;
char mqttDisconnectTime[20];
unsigned long mqttDisconnectTimestamp;
char mqttServer[DNS_LEN];
// unsigned int mqttPort;
char mqttPortStr[6];
char mqttUser[MQTT_LEN];
char mqttPassword[MQTT_LEN];
char mqttTopicPath[MQTT_LEN];
char mqttClientId[STRING_LEN];
static char mqttWillTopic[MQTT_LEN];

Ticker mqttReconnectTimer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
char ntpServer[DNS_LEN];
char ntpTimezone[STRING_LEN];
time_t now;
struct tm localTime;

char hostname[HOSTNAME_LEN];
#define CONFIG_VERSION "3"
Preferences preferences;
int iotWebConfPinState = HIGH;
unsigned long iotWebConfPinChanged = 0;
DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
char impulseCountedStr[10];
IotWebConf iotWebConf("Gaszaehler", &dnsServer, &server, "", CONFIG_VERSION);
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("server", "mqttServer", mqttServer, DNS_LEN);
IotWebConfNumberParameter mqttPortParam = IotWebConfNumberParameter("port", "mqttPort", mqttPortStr, 6, "1883");
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("user", "mqttUser", mqttUser, MQTT_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("password", "mqttPassword", mqttPassword, MQTT_LEN);
IotWebConfTextParameter mqttTopicPathParam = IotWebConfTextParameter("topicpath", "mqttTopicPath", mqttTopicPath, MQTT_LEN, "ht/gas/");
IotWebConfTextParameter mqttClientIdParam = IotWebConfTextParameter("clientname", "mqttClientId", mqttClientId, STRING_LEN);
IotWebConfParameterGroup ntpGroup = IotWebConfParameterGroup("ntp", "NTP");
IotWebConfTextParameter ntpServerParam = IotWebConfTextParameter("server", "ntpServer", ntpServer, DNS_LEN, "de.pool.ntp.org");
IotWebConfTextParameter ntpTimezoneParam = IotWebConfTextParameter("timezone", "ntpTimezone", ntpTimezone, STRING_LEN, "CET-1CEST,M3.5.0/02,M10.5.0/03");
IotWebConfParameterGroup impulseGroup = IotWebConfParameterGroup("impulse", "Impulse");
IotWebConfNumberParameter impulsePinParam = IotWebConfNumberParameter("impulse sensor (GPIO)", "impulsePin", impulsePinStr, 3, "27");
IotWebConfNumberParameter impulseLedParam = IotWebConfNumberParameter("impulse LED (GPIO)", "impulseLed", impulseLedStr, 3, "2");
IotWebConfTextParameter impulseUnitParam = IotWebConfTextParameter("unit", "impulseUnit", impulseUnit, STRING_LEN, "m3");
iotwebconf::FloatTParameter impulseMultiplierParam = iotwebconf::Builder<iotwebconf::FloatTParameter>("impulseMultiplierParam").label("multiplier").defaultValue(1.0).step(0.01).placeholder("e.g. 23.4").build();
IotWebConfNumberParameter impulseCountedParam = IotWebConfNumberParameter("impulses counted", "impulseCounted", impulseCountedStr, 11, "0");
IotWebConfTextParameter impulseMeterIdParam = IotWebConfTextParameter("meter id", "impulseMeterId", impulseMeterId, STRING_LEN);
IotWebConfTextParameter impulseMeterNameParam = IotWebConfTextParameter("meter name", "impulseMeterName", impulseMeterName, STRING_LEN);

/* #region Common functions */
int mod(int x, int y)
{
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}
/* #endregion */

/* #region  Necessary forward declarations*/
void setTimezone(String timezone);
void connectToMqtt();
void mqttPublish(const char *topic, const char *payload, bool force, bool jsonAddTimestamp);
void mqttSendTopics(bool mqttInit);
String getHeartbeatMessage();
/* #endregion */

/* #region ISR */
void handleImpulseChanging1()
{
  if ((millis() - timeDetected) > DEBOUNCE_DELAY)
  {
    if (digitalRead(impulsePin) == LOW)
      impulseCounted++;
    // Serial.print("Impulse detected: ");
    // Serial.println(millis() - timeDetected);
  }
  digitalWrite(LED_BUILTIN, digitalRead(impulsePin));
  timeDetected = millis();
}
/* #endregion*/

/* #region NVS handling*/
void changeNvsMode(bool readOnly)
{
  if (nvsStatus)
  {
    preferences.end();
  }
  if (preferences.begin("settings", readOnly))
    nvsStatus = true;
  else
  {
    Serial.println("Error opening NVS-Namespace");
    for (;;)
      ; // leere Dauerschleife -> Ende
  }
}

void saveImpulseToNvs()
{
  if (nvsImpulseCounted != impulseCounted)
  {
    nvsImpulseCounted = impulseCounted;
    preferences.putUInt("impulseCounter", nvsImpulseCounted);
  }
}

void saveHeartbeatToNvs()
{
  // heartbeat - save NTP time in NVS
  if (timeClient.isTimeSet() && (heartbeatError == 0 || !preferences.isKey("heartbeat")))
  {
    Serial.print("heartbeat saved: ");
    Serial.println(timeClient.getEpochTime());
    preferences.putULong("heartbeat", timeClient.getEpochTime());
  }
}

// TODO: setHistoricalData und Webseite dafür integrieren
void saveHistoricalData()
{
  const int dayOfMonth = 1; // save every first day of month
  char year[5];
  char month[3];
  char day[3];
  String jsonString;
  char timeStr[20];
  itoa(localTime.tm_year + 1900, year, 10);
  itoa(localTime.tm_mon, month, 10);
  itoa(localTime.tm_mday, day, 10);

  Serial.println("Debugging:");
  Serial.println(day);
  Serial.println(dayOfMonth);

  if (timeClient.isTimeSet() && localTime.tm_mday == dayOfMonth)
  {
    if (historicalData[year][month][day].isNull())
    {
      strftime(timeStr, 20, "%d.%m.%Y %T", &localTime);
      // historicalData[year][month][day][0]["thing_pin"] = String(iotWebConf.getThingName()) + "_" + impulsePin;
      historicalData[year][month][day][0]["name"] = impulseMeterName;
      historicalData[year][month][day][0]["id"] = impulseMeterId;
      historicalData[year][month][day][0]["impulse"] = impulseCounted;
      historicalData[year][month][day][0]["value"] = float(impulseCounted) * impulseMultiplierParam.value();
      historicalData[year][month][day][0]["unit"] = impulseUnit;
      historicalData[year][month][day][0]["timestamp"] = timeClient.getEpochTime();
      historicalData[year][month][day][0]["date"] = timeStr;
      Serial.print("Storing historical data: ");
      serializeJsonPretty(historicalData, jsonString);
      Serial.println(jsonString);

      serializeJson(historicalData, jsonString);
      preferences.putString("historicalData", jsonString);
      mqttPublish(MQTT_PUB_HIST, jsonString.c_str(), false, false);
    }
  }
}

// // Funktion zur Authentifizierung
// bool isAuthenticated()
// {
//   if (server.authenticate(http_username, http_password))
//   {
//     return true;
//   }
//   server.requestAuthentication();
//   return false;
// }

// // Funktion zur Behandlung der Root-Seite
// void handleRoot()
// {
//   if (!isAuthenticated())
//   {
//     return;
//   }
//   server.send(200, "text/html", "<h1>Geschützte Seite</h1><p>Willkommen!</p>");
// }

void handleDeleteHistoricalData()
{
  if (!server.authenticate("admin", iotWebConf.getApPasswordParameter()->valueBuffer))
  {
    return server.requestAuthentication();
  }

  historicalData.clear();
  changeNvsMode(false);
  preferences.remove("historicalData");
  changeNvsMode(true);

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>Impulsemeter</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "Historical data deleted! Return to <a href=\"/\">home page</a>.";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
}

void handleViewHistoricalData()
{
  if (!server.authenticate("admin", iotWebConf.getApPasswordParameter()->valueBuffer))
  {
    return server.requestAuthentication();
  }

  if (server.hasArg("json"))
  {
    String jsonString = server.arg("json").c_str();
    Serial.println("Json: " + jsonString);
    DeserializationError deserializationError = deserializeJson(historicalData, jsonString);
    if (deserializationError == DeserializationError::Ok)
    {
      changeNvsMode(false);
      preferences.putString("historicalData", jsonString);
      changeNvsMode(true);
      server.send(200, "text/plain", "Historical Json data saved! Return to <a href=\"/\">home page</a>.");
    }
    else
      server.send(200, "text/plain", "Json has errors! Data not saved! Return to <a href=\"/\">home page</a>.");
  }
  else
  {
    String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
    String jsonString;

    serializeJsonPretty(historicalData, jsonString);

    s += iotWebConf.getHtmlFormatProvider()->getStyle();
    s += "<title>Impulsemeter</title>";
    s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
    s += "<form><label for=\"json\">historical data json:</label><p><textarea id=\"json\" name=\"json\" rows=\"40\" cols=\"60\">";
    s += jsonString;
    s += "</textarea><p><button type=\"submit\">Save</button><p><button type=\"reset\">Reset</button></form>";
    s += iotWebConf.getHtmlFormatProvider()->getEnd();
    server.send(200, "text/html", s);
  }
}
/* #endregion*/

/* #region ESP*/
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
  s += "<title>Impulsemeter</title>";
  s += iotWebConf.getHtmlFormatProvider()->getHeadEnd();
  s += "Crashing in 5 seconds...!";
  s += iotWebConf.getHtmlFormatProvider()->getEnd();
  server.send(200, "text/html", s);
  startCrashTimer(5);
}
/* #endregion*/

/* #region Wifi Manager */
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
  s += "<td>status: </td>";
  if (mqttClient.connected())
    s += "<td>connected</td>";
  else
    s += "<td>disconnected</td>";
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
  strftime(tempStr, 20, "%d.%m.%Y %T", &localTime);
  s += "<td>" + String(tempStr) + "</td>";
  s += "</tr></table></fieldset>";

  s += "<fieldset id=\"status\">";
  s += "<legend>Status</legend>";
  s += "<p>impulse counter: ";
  s += impulseCounted;
  s += "<p>consumption: ";
  s += float(impulseCounted) * impulseMultiplierParam.value();
  s += " ";
  s += impulseUnit;
  s += "<p>meter id: ";
  s += impulseMeterId;
  s += "<p>name: ";
  s += impulseMeterName;
  uptime::calculateUptime();
  sprintf(tempStr, "%04u Tage %02u:%02u:%02u", uptime::getDays(), uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  s += "<p>uptime: " + String(tempStr);
  s += "<p>last reset reason: " + verbose_print_reset_reason(esp_reset_reason());
  s += "<p>heartbeat: ";
  s += getHeartbeatMessage();
  s += "<p>";
  s += "<button onclick=\"if (confirm('Delete history?')) { window.location.href = '/deleteHistoricalData'; }\">delete historical data</button>";
  s += "<p><button onclick=\" window.location.href = '/viewHistoricalData'; \">view historical data</button>";
  // s += "<p>";
  // s += "GPIOs set to sensor: " + String(impulsePin) + ", led: " + String(impulseLed);
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
  s += "<title>Impulsemeter</title>";
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

void configSaved()
{
  // check if Wifi configration has changed - if yes, restart
  changeNvsMode(false);
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

  // TODO: Funktioniert mit apPasswort noch nicht immer....
  Serial.println(iotWebConf.getApPasswordParameter()->getLength() > 0);
  if (iotWebConf.getApPasswordParameter()->getLength() > 0)
    preferences.putString("apPassword", String(iotWebConf.getApPasswordParameter()->valueBuffer));
  preferences.putString("wifiSsid", String(iotWebConf.getWifiAuthInfo().ssid));
  preferences.putString("wifiPassword", String(iotWebConf.getWifiAuthInfo().password));

  if (timeClient.isTimeSet())
  {
    heartbeatError = 0;
    preferences.putULong("heartbeat", timeClient.getEpochTime());
  }

  impulseCounted = atoi(impulseCountedStr);
  saveImpulseToNvs();
  changeNvsMode(true);

  if (impulsePin != atoi(impulsePinStr))
    needReset = true;

  if (impulseLed != atoi(impulseLedStr))
    needReset = true;

  // restart MQTT connection
  mqttClient.disconnect();
  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, atoi(mqttPortStr));
  Serial.println("MQTT ready again");
  connectToMqtt();

  // restart NTP connection
  // configure the timezone
  configTime(0, 0, ntpServer);
  setTimezone(ntpTimezone);
  Serial.println("NTP ready again");

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

  if (!webRequestWrapper->arg(mqttTopicPathParam.getId()).endsWith("/"))
  {
    mqttTopicPathParam.errorMessage = "The topicpath must end with a slash: /";
    valid = false;
  }
  if (!webRequestWrapper->arg(impulseMeterIdParam.getId()).length() > 0)
  {
    impulseMeterIdParam.errorMessage = "The meter id must be set!";
    valid = false;
  }
  if (!webRequestWrapper->arg(impulsePinParam.getId()).toInt() > 0)
  {
    impulsePinParam.errorMessage = "GPIO number must be greater than 0!";
    valid = false;
  }

  if (!webRequestWrapper->arg(impulseLedParam.getId()).toInt() > 0)
  {
    impulseLedParam.errorMessage = "GPIO number must be greater than 0!";
    valid = false;
  }

  return valid;
}
/* #endregion */

/* #region NTP*/
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
/* #endregion */

/* #region MQTT/Status data preparation*/
bool getMqttActive()
{
  return String(mqttServer).length() > 0;
}

String getHeartbeatMessage()
{
  switch (heartbeatError)
  {
  case 0:
    return "ok";
  case 1:
    return "unchecked";
  case 2:
    return "downtime too long";
  }
  return "";
}

String getWifiJson()
{
  JsonDocument object;
  String jsonString;

  object["ssid"] = WiFi.SSID();
  object["sta_ip"] = WiFi.localIP().toString();
  object["rssi"] = WiFi.RSSI();
  object["mac"] = WiFi.macAddress();
  serializeJson(object, jsonString);
  return jsonString;
}

String getSysinfoJson()
{
  JsonDocument object;
  String jsonString;
  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  object["heartbeat"]["code"] = heartbeatError;
  object["heartbeat"]["msg"] = getHeartbeatMessage();
  object["heartbeat"]["downtime"] = downtime; // downtime in seconds
  object["sys"]["reset_reason"] = esp_reset_reason();
  object["sys"]["reset_reason_msg"] = verbose_print_reset_reason(esp_reset_reason());
  object["sys"]["core_dump"] = esp_core_dump_image_check();
  // object["system"]["heap_free"] = esp_get_free_internal_heap_size();    // in bytes
  object["sys"]["heap_min_free"] = esp_get_minimum_free_heap_size(); // in bytes
  object["sys"]["nvs_entries_pct"] = nvs_stats.used_entries / nvs_stats.total_entries * 100;
  object["ntp"]["time_set"] = timeClient.isTimeSet();
  object["mqtt"]["disconnect_reason"] = mqttDisconnectReason;
  object["mqtt"]["disconnect_time"] = mqttDisconnectTime;
  object["mqtt"]["disconnect_timestamp"] = mqttDisconnectTimestamp;

  serializeJson(object, jsonString);
  return jsonString;
}

String getHistoricalDataJson()
{
  String jsonString;
  serializeJson(historicalData, jsonString);
  return jsonString;
}

String getActualDataJson()
{
  JsonDocument object;
  String jsonString;

  // object["thing_pin"] = String(iotWebConf.getThingName()) + "_" + impulsePin; // generate a simple unique ID
  object["name"] = impulseMeterName;
  object["id"] = impulseMeterId;
  object["impulse"] = impulseCounted;
  object["value"] = float(impulseCounted) * impulseMultiplierParam.value();
  object["unit"] = impulseUnit;
  serializeJson(object, jsonString);
  return jsonString;
}

void mqttPublishInfo(String info)
{
  mqttPublish(MQTT_PUB_INFO, info.c_str(), false, false);
}

void mqttPublishUptime()
{
  char msg_out[20];
  uptime::calculateUptime();
  sprintf(msg_out, "%04u %s %02u:%02u:%02u", uptime::getDays(), "days", uptime::getHours(), uptime::getMinutes(), uptime::getSeconds());
  // Serial.println(msg_out);
  mqttPublish(MQTT_PUB_INFO, msg_out, false, false);
}

void mqttSendTopics(bool mqttInit)
{
  // TODO: mit dem Timestamp funktioniert die Erkennung auf Änderung nicht mehr. Hier noch einbauen.
  if (timeClient.isTimeSet())
    mqttPublish(String(String(MQTT_PUB_ACT) + "/" + String(impulseMeterId)).c_str(), getActualDataJson().c_str(), mqttInit, true);
  if (!historicalData.isNull())
    mqttPublish(MQTT_PUB_HIST, getHistoricalDataJson().c_str(), mqttInit, false);
  mqttPublish(MQTT_PUB_SYSINFO, getSysinfoJson().c_str(), mqttInit, true);
}

/* #endregion*/

/* #region connection handling*/
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
  mqttPublish(MQTT_PUB_STATUS, "Online", true, false);
  mqttPublish(MQTT_PUB_WIFI, getWifiJson().c_str(), true, true);
  uint16_t packetIdSub;
  packetIdSub = mqttClient.subscribe((String(mqttTopicPath) + String(MQTT_SUB_CMND_IMPULSE)).c_str(), 0);
  Serial.print("Subscribed to topic: ");
  Serial.println(String(mqttTopicPath) + String(MQTT_SUB_CMND_IMPULSE) + " - " + String(packetIdSub));
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
  strftime(mqttDisconnectTime, 20, "%d.%m.%Y %T", &localTime);
  mqttDisconnectTimestamp = timeClient.getEpochTime();
  Serial.printf(" [%8u] Disconnected from the broker reason = %s\n", millis(), mqttDisconnectReason.c_str());
  if (WiFi.isConnected())
  {
    Serial.printf(" [%8u] Reconnecting to MQTT..\n", millis());
    // timer.in(5000, connectToMqtt);
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
  char new_payload[len + 1];
  strncpy(new_payload, payload, len);
  new_payload[len] = '\0';
  if (String(topic) == (String(mqttTopicPath) + String(MQTT_SUB_CMND_IMPULSE)))
  {
    Serial.print("MQTT command received to set impulse counter: ");
    Serial.println(new_payload);
    JsonDocument object;
    if (DeserializationError::Ok == deserializeJson(object, new_payload))
    {
      if (!object["impulse"].isNull())
      {
        impulseCounted = object["impulse"];
        changeNvsMode(false);
        saveImpulseToNvs();
        Serial.print("Impulse set to: ");
        Serial.println(impulseCounted);
        if (timeClient.isTimeSet())
        {
          heartbeatError = 0;
          preferences.putULong("heartbeat", timeClient.getEpochTime());
        }
        changeNvsMode(true);
      }
    }
    else
    {
      mqttPublishInfo("Deserialization of JsonObject for topic " + String(topic) + " not n´successfull.");
      Serial.print("Deserialization of JsonObject for topic " + String(topic) + " not n´successfull: ");
      Serial.println(new_payload);
    }
  }
}

void mqttPublish(const char *topic, const char *payload, bool force, bool jsonAddTimstamp)
{
  static std::map<String, String> mqttLastMessage;
  if (getMqttActive())
  {
    String topicStr = String(topic);
    String payloadStr = String(payload);
    String newPayloadStr = String(payload);
    String tempTopicStr = String(mqttTopicPath) + String(topic);

    if (mqttClient.connected())
    {
      if (mqttLastMessage[topicStr] != payloadStr || force)
      {
        if (jsonAddTimstamp && timeClient.isTimeSet())
        {
          JsonDocument object;
          char timeStr[20];
          strftime(timeStr, 20, "%d.%m.%Y %T", &localTime);
          deserializeJson(object, payload);
          object["timestamp"] = timeClient.getEpochTime();
          object["date"] = timeStr;
          serializeJson(object, newPayloadStr);
        }
        Serial.println("MQTT send: " + tempTopicStr + " = " + newPayloadStr);
        if (mqttClient.publish(tempTopicStr.c_str(), 0, true, newPayloadStr.c_str()) > 0)
          // TODO: Statt dem String ggf. einen Hash wegspeichern zur Optimierung der Speichernutzung
          mqttLastMessage[topicStr] = payloadStr;
        else
          Serial.println("MQTT (error) not send: " + tempTopicStr + " = " + newPayloadStr);
      }
    }
    else
    {
      Serial.println("MQTT not send: " + tempTopicStr + " = " + payloadStr);
    }
  }
}
/* #endregion*/

bool onSec10Timer(void *)
{
  // check heartbeat and set errorstate - check onetime if NTP is available the first time
  if (timeClient.isTimeSet() && heartbeatError == 1 && preferences.isKey("heartbeat"))
  {
    Serial.print("heartbeat: ");
    Serial.println(preferences.getULong("heartbeat"));
    downtime = timeClient.getEpochTime() - preferences.getULong("heartbeat") - (millis() / 1000);
    if (downtime > (MAX_DOWNTIME + (millis() / 1000))) // 10 minutes -default- not running leads into an error message
      heartbeatError = 2;
    else
      heartbeatError = 0;
    char msg_out[20];
    sprintf(msg_out, "%d", downtime);
    Serial.print("Downtime detected: ");
    Serial.println(msg_out);
  }
  mqttSendTopics(false);

  return true;
}

bool onMin5Timer(void *)
{
  mqttPublishUptime();
  mqttPublish(MQTT_PUB_WIFI, getWifiJson().c_str(), false, true);
  changeNvsMode(false);
  saveImpulseToNvs();
  saveHistoricalData();
  saveHeartbeatToNvs();
  changeNvsMode(true);

  return true;
}

void setup()
{
  // basic setup
  Serial.begin(115200);
  esp_core_dump_init();

  // WiFi.onEvent(onWifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Start NVS configuration
  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  Serial.println("NVS-Statistics:");
  Serial.print("Used entries: ");
  Serial.println(nvs_stats.used_entries);
  Serial.print("Free entries: ");
  Serial.println(nvs_stats.free_entries);
  Serial.print("Total entries: ");
  Serial.println(nvs_stats.total_entries);

  changeNvsMode(false);
  if (preferences.isKey("historicalData"))
  {
    deserializeJson(historicalData, preferences.getString("historicalData"));
    Serial.println("Loaded historical data");
    // Test of deserialization
    // String JsonString;
    // serializeJsonPretty(historicalData, JsonString);
    // Serial.println(JsonString);
  }
  else
    Serial.println("Historical data not found in NVRAM");

  impulseCounted = preferences.getUInt("impulseCounter", 0);
  nvsImpulseCounted = impulseCounted;

  iotWebConf.setupUpdateServer(
      [](const char *updatePath)
      { httpUpdater.setup(&server, updatePath); },
      [](const char *userName, char *password)
      { httpUpdater.updateCredentials(userName, password); });

  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttPortParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  mqttGroup.addItem(&mqttClientIdParam);
  mqttGroup.addItem(&mqttTopicPathParam);
  iotWebConf.addParameterGroup(&mqttGroup);
  ntpGroup.addItem(&ntpServerParam);
  ntpGroup.addItem(&ntpTimezoneParam);
  iotWebConf.addParameterGroup(&ntpGroup);
  impulseGroup.addItem(&impulsePinParam);
  impulseGroup.addItem(&impulseLedParam);
  impulseGroup.addItem(&impulseUnitParam);
  impulseGroup.addItem(&impulseMultiplierParam);
  impulseGroup.addItem(&impulseCountedParam);
  impulseGroup.addItem(&impulseMeterIdParam);
  impulseGroup.addItem(&impulseMeterNameParam);
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
  // server.on("/crash", startCrash); // Debugging only
  server.on("/deleteHistoricalData", handleDeleteHistoricalData);
  server.on("/viewHistoricalData", handleViewHistoricalData);
  impulsePin = atoi(impulsePinStr);
  impulseLed = atoi(impulseLedStr);
  Serial.println("Wifi manager ready.");

  // Init MQTT
  if (!mqttClientId)
    mqttClient.setClientId(iotWebConf.getThingName());
  strcpy(mqttWillTopic, mqttTopicPath);
  strcat(mqttWillTopic, MQTT_PUB_STATUS);
  mqttClient.setWill(mqttWillTopic, 0, true, "Offline", 7);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onSubscribe(onMqttSubscribe);

  // TODO: Add SSL connection
  if (mqttUser != "")
    mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setServer(mqttServer, atoi(mqttPortStr));
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
                   { 
                   Serial.println("\nEnd OTA"); 
                   changeNvsMode(false);
                   saveImpulseToNvs();
                   saveHeartbeatToNvs();
                   changeNvsMode(true); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
  Serial.println("OTA ready");

  changeNvsMode(true);
  // Timers
  timer.every(10000, onSec10Timer);
  timer.every(300000, onMin5Timer);
  Serial.println("Timer ready");

  // PINs
  pinMode(impulseLed, OUTPUT);
  digitalWrite(impulseLed, LOW);
  pinMode(impulsePin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(impulsePin), handleImpulseChanging1, CHANGE);
  Serial.println("GPIOs set to sensor: " + String(impulsePin) + ", LED: " + String(impulseLed));
  Serial.println("ISR ready");
}

void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  updateTime();
  timer.tick();
  if (needReset)
  {
    Serial.println("Rebooting in 1 second.");
    changeNvsMode(false);
    saveImpulseToNvs();
    saveHeartbeatToNvs();
    changeNvsMode(true);
    iotWebConf.delay(1000);
    ESP.restart();
  }
}