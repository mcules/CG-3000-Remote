#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <time.h>
#include <AsyncMqttClient.h>
#include <ESPmDNS.h>

WebServer server(80);

WiFiManager wm;

AsyncMqttClient mqttClient;

const char *MQTT_HOST = "mqtt.ham.local";
const uint16_t MQTT_PORT = 1883;

IPAddress g_mqttIp;
bool g_mqttIpValid = false;
unsigned long g_lastDnsAttempt = 0;
uint32_t g_dnsBackoffMs = 5000;
const uint32_t DNS_BACKOFF_MAX = 60000;

const char *HAM_DISCOVERY_PREFIX = "ham";
String g_deviceId;
unsigned long g_lastMqttStatePublish = 0;
bool g_lastPowerPublished = false;
bool g_lastTuningPublished = false;
String g_lastLastResetStr;
String g_lastTimeStr;
bool g_mqttInitialPublishDone = false;
const unsigned long MQTT_HEARTBEAT_MS = 5UL * 60UL * 1000UL; // 5 minutes
const unsigned long MQTT_CHECK_INTERVAL_MS = 1000UL;         // Proofintervall in loop()
unsigned long g_lastMqttHeartbeat = 0;

String chipId()
{
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

String deviceBaseTopic()
{
  return "cg3000/" + g_deviceId; // z.B. cg3000/cg3000-ABCDEF
}

bool shouldRestart = false;
#define PIN_STATUS_TUNING 34
#define PIN_RELAY_RESET 16
#define PIN_RELAY_POWER 17

time_t g_lastReset = 0;
unsigned long g_bootMillis = 0;
bool g_timeValid = false;

String formatTime(time_t t)
{
  if (t <= 0)
    return String("-");
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S %d.%m.%Y", &tmInfo);
  return String(buf);
}

time_t nowSec()
{
  time_t now;
  time(&now);
  return now;
}

bool isTimeValid()
{
  struct tm tmInfo;
  if (!getLocalTime(&tmInfo, 10))
    return false;
  return (tmInfo.tm_year + 1900) >= 2020;
}

String formatUptime(unsigned long ms)
{
  unsigned long sec = ms / 1000UL;
  unsigned long days = sec / 86400UL;
  sec %= 86400UL;
  unsigned long hrs = sec / 3600UL;
  sec %= 3600UL;
  unsigned long mins = sec / 60UL;
  sec %= 60UL;

  char buf[48];
  if (days > 0)
  {
    snprintf(buf, sizeof(buf), "%lu d %lu h %lu m %lu s", days, hrs, mins, sec);
  }
  else if (hrs > 0)
  {
    snprintf(buf, sizeof(buf), "%lu h %lu m %lu s", hrs, mins, sec);
  }
  else if (mins > 0)
  {
    snprintf(buf, sizeof(buf), "%lu m %lu s", mins, sec);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%lu s", sec);
  }
  return String(buf);
}

String htmlPage()
{
  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>CG-3000 Remote</title>";
  html += "<style>";
  html += ":root{--bg:#0f172a;--card:#111827;--muted:#94a3b8;--ok:#22c55e;--err:#ef4444;--text:#e5e7eb;}";
  html += "html,body{height:100%;margin:0;padding:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}";
  html += ".wrap{max-width:780px;margin:0 auto;padding:24px;}";
  html += ".card{background:var(--card);border-radius:14px;box-shadow:0 6px 24px rgba(0,0,0,.35);padding:22px;}";
  html += "h1{margin:0 0 8px 0;font-size:24px;font-weight:700}";
  html += ".sub{color:var(--muted);font-size:14px;margin-bottom:18px}";
  html += ".grid{display:grid;grid-template-columns:repeat(2,minmax(240px,1fr));gap:18px}";
  html += ".kpi{background:#0b1220;border:1px solid #1f2937;border-radius:12px;padding:14px;position:relative}";
  html += ".kpi .label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}";
  html += ".kpi .value{margin-top:6px;font-size:18px}";
  html += ".muted{color:var(--muted)}";
  html += ".badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:12px;font-weight:600}";
  html += ".badge.on{background:rgba(34,197,94,.12);color:var(--ok);border:1px solid rgba(34,197,94,.35)}";
  html += ".badge.off{background:rgba(239,68,68,.12);color:var(--err);border:1px solid rgba(239,68,68,.35)}";
  /* Reset icon button inside Last Reset card (top-right) */
  html += ".icon-btn{position:absolute;top:10px;right:10px;display:inline-flex;align-items:center;justify-content:center;width:32px;height:32px;border-radius:8px;border:1px solid #374151;background:#1f2937;color:#e5e7eb;cursor:pointer}";
  html += ".icon-btn:hover{background:#273244}";
  html += ".icon{width:16px;height:16px;display:inline-block}";
  /* MQTT icon */
  html += ".topbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}";
  html += ".mqtt-ind{position:relative;display:inline-flex;align-items:center;gap:8px;}";
  html += ".mqtt-dot{width:12px;height:12px;border-radius:50%;box-shadow:0 0 0 3px rgba(255,255,255,0.06) inset}";
  html += ".mqtt-on{background:#22c55e}";  // grün
  html += ".mqtt-off{background:#3b82f6}"; // blau
  html += ".mqtt-label{font-size:12px;color:var(--muted)}";
  /* Toggle Switch visuals (inside Power card) */
  html += ".switch-wrap{display:flex;align-items:center;gap:10px;margin-top:6px}";
  html += ".tgl{appearance:none;-webkit-appearance:none;width:58px;height:32px;background:#b91c1c;border-radius:999px;position:relative;outline:none;cursor:pointer;transition:background .2s ease, box-shadow .2s ease;border:1px solid rgba(0,0,0,.35)}";
  html += ".tgl:before{content:\"\";position:absolute;top:3px;left:3px;width:26px;height:26px;background:#fff;border-radius:50%;transition:transform .2s ease, box-shadow .2s ease;box-shadow:0 1px 2px rgba(0,0,0,.35)}";
  html += ".tgl:checked{background:#16a34a}";
  html += ".tgl:checked:before{transform:translateX(26px)}";
  html += ".state{min-width:42px;text-align:center;font-weight:700}";
  html += ".state.on{color:#22c55e}.state.off{color:#ef4444}";
  html += "@media (max-width:560px){.grid{grid-template-columns:1fr}}";
  html += "</style>";
  html += "<script>";
  html += "let busy=false;";
  html += "async function fetchStatus(){";
  html += "  try{const r=await fetch('/status',{cache:'no-store'});const s=await r.json();";
  html += "    const power=s.power===true;const tuning=s.tuning===true;";
  html += "    const tuneBadge=document.getElementById('tuningBadge');";
  html += "    tuneBadge.className='badge '+(tuning?'on':'off');";
  html += "    tuneBadge.textContent=tuning?'ACTIVE':'IDLE';";
  html += "    document.getElementById('lastReset').textContent=s.lastResetStr||'-';";
  html += "    document.getElementById('now').textContent=s.timeStr||'-';";
  html += "    const tgl=document.getElementById('powerToggle');";
  html += "    if(!busy){tgl.checked=power;}";
  html += "    const stateEl=document.getElementById('powerState');";
  html += "    stateEl.textContent=power?'ON':'OFF';";
  html += "    stateEl.className='state ' + (power?'on':'off');";
  html += "    const mqttDot=document.getElementById('mqttDot');";
  html += "    if(s.mqttConnected===true){";
  html += "      mqttDot.className='mqtt-dot mqtt-on';";
  html += "    }else{";
  html += "      mqttDot.className='mqtt-dot mqtt-off';";
  html += "    }";
  html += "  }catch(e){}";
  html += "  setTimeout(fetchStatus,2000);";
  html += "}";
  html += "async function togglePower(ev){";
  html += "  if(busy) return; busy=true;";
  html += "  const tgl=ev.currentTarget; tgl.disabled=true;";
  html += "  try{await fetch('/power',{method:'POST'});";
  html += "      setTimeout(fetchStatus,300);";
  html += "  }catch(e){}";
  html += "  finally{tgl.disabled=false; busy=false;}";
  html += "}";
  html += "function doReset(){";
  html += "  const btn=document.getElementById('btnReset'); btn.disabled=true;";
  html += "  fetch('/reset',{method:'POST'}).then(()=>setTimeout(()=>{btn.disabled=false;},1200)).catch(()=>{btn.disabled=false;});";
  html += "}";
  html += "window.addEventListener('load',()=>{";
  html += "  document.getElementById('powerToggle').addEventListener('change',togglePower);";
  html += "  const st=document.getElementById('powerState'); st.className='state off';";
  html += "  fetchStatus();";
  html += "});";
  html += "</script>";
  html += "</head><body>";
  html += "  <div class='wrap'>";
  html += "    <div class='card'>";
  html += "      <div class='topbar'>";
  html += "        <div>";
  html += "          <h1>CG-3000 Antenna Tuner</h1>";
  html += "          <div class='sub'>Status and Control</div>";
  html += "        </div>";
  html += "        <div class='mqtt-ind' title='MQTT connection'>";
  html += "          <span id='mqttDot' class='mqtt-dot mqtt-off'></span>";
  html += "          <span id='mqttText' class='mqtt-label'>MQTT</span>";
  html += "        </div>";
  html += "      </div>";
  html += "      <div class='grid'>";
  // Row 1: top-left
  html += "        <div class='kpi'>";
  html += "          <div class='label'>Power</div>";
  html += "          <div class='switch-wrap'>";
  html += "            <input id='powerToggle' class='tgl' type='checkbox'/>";
  html += "            <div id='powerState' class='state'>OFF</div>";
  html += "          </div>";
  html += "        </div>";
  // Row 1: top-right
  html += "        <div class='kpi'>";
  html += "          <div class='label'>Time</div>";
  html += "          <div class='value' id='now'>-</div>";
  html += "          <div class='muted' style='font-size:12px;margin-top:4px'>(NTP time or uptime)</div>";
  html += "        </div>";
  // Row 2: bottom-left
  html += "        <div class='kpi'>";
  html += "          <div class='label'>Tuning</div>";
  html += "          <div class='value'><span id='tuningBadge' class='badge off'>IDLE</span></div>";
  html += "        </div>";
  // Row 2: bottom-right
  html += "        <div class='kpi'>";
  html += "          <div class='label'>Last Reset</div>";
  html += "          <button id='btnReset' class='icon-btn' onclick='doReset()' title='Trigger reset'>";
  html += "            <span class='icon' aria-hidden='true'>";
  html += "              <svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' width='16' height='16'>";
  html += "                <polyline points='23 4 23 10 17 10'></polyline>";
  html += "                <path d='M20.49 15a9 9 0 1 1 2.13-9'></path>";
  html += "              </svg>";
  html += "            </span>";
  html += "          </button>";
  html += "          <div class='value' id='lastReset'>-</div>";
  html += "        </div>";
  // end grid
  html += "      </div>";
  html += "    </div>";
  html += "  </div>";
  html += "</body></html>";
  return html;
}

bool resolveMqttHostNonBlocking()
{
  unsigned long nowMs = millis();
  if (g_mqttIpValid)
    return true;
  if (nowMs - g_lastDnsAttempt < g_dnsBackoffMs)
    return false;

  g_lastDnsAttempt = nowMs;

  IPAddress mdnsIp = MDNS.queryHost(MQTT_HOST);
  if ((uint32_t)mdnsIp != 0)
  {
    g_mqttIp = mdnsIp;
    g_mqttIpValid = true;
    g_dnsBackoffMs = 5000;
    return true;
  }

  g_dnsBackoffMs = min(DNS_BACKOFF_MAX, g_dnsBackoffMs * 2);
  return false;
}

bool mqttPublishState(bool forceAll = false)
{
  if (!mqttClient.connected())
    return false;

  String base = deviceBaseTopic();
  bool power = (digitalRead(PIN_RELAY_POWER) == HIGH);
  bool tuning = digitalRead(PIN_STATUS_TUNING);

  g_timeValid = isTimeValid();
  time_t now = nowSec();
  String timeStr = g_timeValid ? formatTime(now) : ("Since Restart: " + formatUptime(millis() - g_bootMillis));
  String lastResetStr = g_timeValid ? formatTime(g_lastReset) : String("-");

  String tPower = base + "/power/state";
  String tTune = base + "/tuning/state";
  String tReset = base + "/lastReset/str";
  String tTime = base + "/time/str";

  bool published = false;

  // Power
  if (forceAll || !g_mqttInitialPublishDone || g_lastPowerPublished != power)
  {
    mqttClient.publish(tPower.c_str(), 0, true, power ? "ON" : "OFF");
    g_lastPowerPublished = power;
    published = true;
  }

  // Tuning
  if (forceAll || !g_mqttInitialPublishDone || g_lastTuningPublished != tuning)
  {
    mqttClient.publish(tTune.c_str(), 0, true, tuning ? "ON" : "OFF");
    g_lastTuningPublished = tuning;
    published = true;
  }

  // Last Reset (String-Cache)
  if (forceAll || g_lastLastResetStr != lastResetStr)
  {
    g_lastLastResetStr = lastResetStr;
    mqttClient.publish(tReset.c_str(), 0, true, lastResetStr.c_str());
    published = true;
  }

  // Time (String-Cache)
  if (forceAll || g_lastTimeStr != timeStr)
  {
    g_lastTimeStr = timeStr;
    mqttClient.publish(tTime.c_str(), 0, true, timeStr.c_str());
    published = true;
  }

  g_mqttInitialPublishDone = true;
  return published;
}

void mqttPublishDiscovery()
{
  String dev = String("{\"id\":\"") + g_deviceId + "\","
                                                   "\"manufacturer\":\"CG-3000\",\"model\":\"ESP32 Remote\",\"name\":\"CG-3000 Remote\"}";
  String base = deviceBaseTopic();

  {
    String objId = g_deviceId + "_power";
    String topic = String(HAM_DISCOVERY_PREFIX) + "/switch/" + objId + "/config";
    String payload = String("{") + "\"id\":\"" + objId + "\"," + "\"cmd\":\"" + base + "/power/set\"," + "\"state\":\"" + base + "/power/state\"," + "\"device\":" + dev + "}";
    mqttClient.publish(topic.c_str(), 0, true, payload.c_str());
  }

  {
    String objId = g_deviceId + "_reset";
    String topic = String(HAM_DISCOVERY_PREFIX) + "/button/" + objId + "/config";
    String payload = String("{") + "\"id\":\"" + objId + "\"," + "\"cmd\":\"" + base + "/reset/set\"," + "\"device\":" + dev + "}";
    mqttClient.publish(topic.c_str(), 0, true, payload.c_str());
  }

  {
    String objId = g_deviceId + "_tuning";
    String topic = String(HAM_DISCOVERY_PREFIX) + "/binary_sensor/" + objId + "/config";
    String payload = String("{") + "\"id\":\"" + objId + "\"," + "\"state\":\"" + base + "/tuning/state\"," + "\"device\":" + dev + "}";
    mqttClient.publish(topic.c_str(), 0, true, payload.c_str());
  }

  {
    String objId = g_deviceId + "_lastreset";
    String topic = String(HAM_DISCOVERY_PREFIX) + "/sensor/" + objId + "/config";
    String payload = String("{") + "\"id\":\"" + objId + "\"," + "\"state\":\"" + base + "/lastReset/str\"," + "\"type\":\"timestamp\"," + "\"device\":" + dev + "}";
    mqttClient.publish(topic.c_str(), 0, true, payload.c_str());
  }

  {
    String objId = g_deviceId + "_time";
    String topic = String(HAM_DISCOVERY_PREFIX) + "/sensor/" + objId + "/config";
    String payload = String("{") + "\"id\":\"" + objId + "\"," + "\"state\":\"" + base + "/time/str\"," + "\"type\":\"timestamp\"," + "\"device\":" + dev + "}";
    mqttClient.publish(topic.c_str(), 0, true, payload.c_str());
  }
}

void onMqttConnect(bool sessionPresent)
{
  String base = deviceBaseTopic();
  mqttClient.subscribe(String(base + "/power/set").c_str(), 0);
  mqttClient.subscribe(String(base + "/reset/set").c_str(), 0);

  mqttPublishDiscovery();

  g_mqttInitialPublishDone = false;
  mqttPublishState(true);         // alles initial senden
  g_lastMqttHeartbeat = millis(); // Heartbeat-Timer starten
  Serial.println("MQTT connected");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  if (reason == AsyncMqttClientDisconnectReason::TCP_DISCONNECTED || reason == AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT)
  {
    g_mqttIpValid = false;
    g_dnsBackoffMs = 5000;
  }
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
  String t(topic);
  String msg;
  msg.reserve(len + 1);
  for (size_t i = 0; i < len; ++i)
    msg += payload[i];

  String base = deviceBaseTopic();

  if (t == base + "/power/set")
  {
    int current = digitalRead(PIN_RELAY_POWER);
    bool newState = (current == HIGH);
    if (msg == "TOGGLE")
      newState = !newState;
    else if (msg == "ON")
      newState = true;
    else if (msg == "OFF")
      newState = false;
    else
      return;

    if (((digitalRead(PIN_RELAY_POWER) == HIGH) != newState))
    {
      digitalWrite(PIN_RELAY_POWER, newState ? HIGH : LOW);
    }
    if (g_lastPowerPublished != newState || !g_mqttInitialPublishDone)
    {
      String baseTopic = deviceBaseTopic();
      mqttClient.publish(String(baseTopic + "/power/state").c_str(), 0, true, newState ? "ON" : "OFF");
      g_lastPowerPublished = newState;
      g_mqttInitialPublishDone = true;
    }
    return;
  }

  if (t == base + "/reset/set")
  {
    digitalWrite(PIN_RELAY_RESET, HIGH);
    delay(250);
    digitalWrite(PIN_RELAY_RESET, LOW);
    g_lastReset = nowSec();
    mqttPublishState();
    return;
  }
}

void handleRoot()
{
  server.send(200, "text/html", htmlPage());
}

void handleStatus()
{
  bool power = (digitalRead(PIN_RELAY_POWER) == HIGH);
  bool tuning = digitalRead(PIN_STATUS_TUNING);

  g_timeValid = isTimeValid();

  time_t now = nowSec();
  String timeStr;
  if (g_timeValid) {
    timeStr = formatTime(now);
  } else {
    timeStr = "Since Restart: " + formatUptime(millis() - g_bootMillis);
  }

  String lastResetStr = g_timeValid ? formatTime(g_lastReset) : String("-");

  String json = "{";
  json += "\"power\":" + String(power ? "true" : "false") + ",";
  json += "\"tuning\":" + String(tuning ? "true" : "false") + ",";
  json += "\"lastReset\":" + String((long)g_lastReset) + ",";
  json += "\"time\":" + String((long)now) + ",";
  json += "\"lastResetStr\":\"" + lastResetStr + "\",";
  json += "\"timeValid\":" + String(g_timeValid ? "true" : "false") + ",";
  json += "\"timeStr\":\"" + timeStr + "\",";
  json += "\"mqttConnected\":" + String(mqttClient.connected() ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleReset()
{
  digitalWrite(PIN_RELAY_RESET, HIGH);
  delay(250);
  digitalWrite(PIN_RELAY_RESET, LOW);
  g_lastReset = nowSec();
  if (mqttClient.connected())
    mqttPublishState();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePower()
{
  int current = digitalRead(PIN_RELAY_POWER);
  digitalWrite(PIN_RELAY_POWER, current == HIGH ? LOW : HIGH);
  if (mqttClient.connected())
    mqttPublishState();
  server.sendHeader("Location", "/");
  server.send(303);
}

void saveConfigCallback()
{
  Serial.println("New WiFi config saved, please restart...");
  shouldRestart = true;
}

void WiFiEvent(WiFiEvent_t event)
{
  if (event == WIFI_EVENT_STA_DISCONNECTED)
  {
    g_mqttIpValid = false;
    g_dnsBackoffMs = 5000;
  }
  if (event == IP_EVENT_STA_GOT_IP)
  {
    // try DNS again on next loop.
  }
}

void setup()
{
  Serial.begin(115200);
  g_bootMillis = millis();

  pinMode(PIN_STATUS_TUNING, INPUT);
  pinMode(PIN_RELAY_RESET, OUTPUT);
  pinMode(PIN_RELAY_POWER, OUTPUT);

  digitalWrite(PIN_RELAY_RESET, LOW);
  digitalWrite(PIN_RELAY_POWER, LOW);

  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setHostname("CG3000-ESP32");
  wm.setConnectRetries(3);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);
  wm.setEnableConfigPortal(true);

  if (!wm.autoConnect("CG3000-Setup", "tuner1234"))
  {
    Serial.println("AP-Modus active.");
  }
  else
  {
    Serial.println("WiFi connected.");

    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

    struct tm tmInfo;
    for (int i = 0; i < 20; ++i)
    {
      if (getLocalTime(&tmInfo, 500))
        break;
      delay(200);
    }
    g_timeValid = isTimeValid();
    Serial.printf("Time valid: %s\n", g_timeValid ? "yes" : "no");
  }

  WiFi.onEvent(WiFiEvent);

  if (shouldRestart)
  {
    delay(1000);
    ESP.restart();
  }

  g_deviceId = "cg3000-" + chipId();

  if (!MDNS.begin("cg3000-esp32"))
  {
    // still silent to avoid spam
  }

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/power", HTTP_POST, handlePower);

  server.onNotFound([]()
                    {
    Serial.printf("404 for: %s %s\n",
                  server.method() == HTTP_GET ? "GET" :
                  server.method() == HTTP_POST ? "POST" : "OTHER",
                  server.uri().c_str());
    server.send(404, "text/plain", "Not found"); });

  server.begin();
}

void loop()
{
  server.handleClient();

  if (!g_mqttIpValid)
  {
    resolveMqttHostNonBlocking();
  }

  if (g_mqttIpValid && !mqttClient.connected())
  {
    mqttClient.setServer(g_mqttIp, MQTT_PORT);
    mqttClient.connect();
  }

  unsigned long nowMs = millis();
  static unsigned long lastCheck = 0;

  if (mqttClient.connected())
  {
    if (nowMs - lastCheck >= MQTT_CHECK_INTERVAL_MS)
    {
      lastCheck = nowMs;

      bool forceAll = (nowMs - g_lastMqttHeartbeat >= MQTT_HEARTBEAT_MS);
      bool didPublish = mqttPublishState(forceAll);

      if (forceAll && didPublish)
      {
        g_lastMqttHeartbeat = nowMs;
      }
    }
  }
}
