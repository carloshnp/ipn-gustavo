/*
  Mega 2560 single-board version (ATmega2560 + ESP8266 placeholder)
  - SD logger shield (CS=10), SPI via ICSP
  - LCD + buttons on remapped pins
  - 2x DHT22
  - 4 relays
  - CSV streamed step-by-step
  - WiFi placeholder on Serial1 (not used now)
*/
#include <Arduino.h>
#include <LiquidCrystal.h>
#include <SPI.h>
#include <SD.h>
#include <DHT.h>
#include <EEPROM.h>
#include <Wire.h>
#include <RTClib.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>

// ===== Pin map (Mega) =====
// SD shield uses CS=10, SPI on ICSP
const byte SD_CS = 10;

// LCD pins (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(30, 31, 32, 33, 34, 35);

// Buttons
const byte BTN_UP   = 22;
const byte BTN_DOWN = 23;
const byte BTN_OK   = 24;
const byte BTN_BACK = 25;

// DHT22 sensors
const byte DHT1_PIN = 26;
const byte DHT2_PIN = 27;
const byte DHTTYPE = DHT22;
const bool USE_DHT2 = false; // set true when second DHT is wired
const bool DHT_USE_PULLUP = false; // set true only if no external pull-up resistor
DHT dht1(DHT1_PIN, DHTTYPE);
DHT dht2(DHT2_PIN, DHTTYPE);

// Relays (bit0 lamp, bit1 fan, bit2 heater, bit3 spray)
const byte RELAY_PINS[4] = {40, 41, 42, 43};
const bool RELAY_ACTIVE_LOW = true; // set false if your relay module is active HIGH

// ===== UI + buttons =====
struct Btn { byte pin; bool stable; bool last; unsigned long t; };
const unsigned long DB_MS = 30;
Btn bU = {BTN_UP,   HIGH, HIGH, 0};
Btn bD = {BTN_DOWN, HIGH, HIGH, 0};
Btn bO = {BTN_OK,   HIGH, HIGH, 0};
Btn bB = {BTN_BACK, HIGH, HIGH, 0};

bool edge(Btn &b) {
  bool r = digitalRead(b.pin);
  if (r != b.last) { b.last = r; b.t = millis(); }
  if (millis() - b.t > DB_MS && r != b.stable) { b.stable = r; return true; }
  return false;
}
inline bool pressed(const Btn &b) { return b.stable == LOW; }

enum UiScreen {
  SCREEN_MENU,
  SCREEN_EXP_LIST,
  SCREEN_INT_LIST,
  SCREEN_SERVICE_MENU,
  SCREEN_CONFIG_MENU,
  SCREEN_CONFIG_HEATER_INTERVAL,
  SCREEN_TIME_SET,
  SCREEN_SENSOR_TEST,
  SCREEN_RELAY_TEST,
  SCREEN_WIFI_STATUS,
  SCREEN_RUNNING,
  SCREEN_CONFIRM_STOP,
  SCREEN_RETRIEVAL
};
UiScreen screen = SCREEN_MENU;
const char* MENU_ITEMS[] = { "Exp SD", "Exp Interno", "Servico", "Ajustar Hora" };
const uint8_t NITEMS = 4;
uint8_t menuIndex = 0;
const char* SERVICE_ITEMS[] = { "Sensores", "Reles", "Cfg Aqec", "WiFi Status", "Recarregar CFG" };
const uint8_t NSERVICE = 5;
uint8_t serviceIndex = 0;
bool sensorCfgPage = false;
const char* CONFIG_ITEMS[] = { "Intervalo Aqec" };
const uint8_t NCONFIG = 1;
uint8_t configIndex = 0;
uint16_t heaterIntervalEdit = 10;

// ===== Experiment data =====
struct StepData {
  char label[10];
  uint16_t seconds;
  uint8_t mask;
  uint16_t tmin10;
  uint16_t tmax10;
};
struct Meta {
  char id[16];
  uint8_t program;
  uint8_t retrievals;
  uint16_t intervalMin;
  uint16_t stepCount;
  uint16_t stepUnitMs;
};
Meta meta;
const uint8_t MAX_FILES = 9; // 6 SD + 3 internal
char expFiles[MAX_FILES][13];
bool expIsInternal[MAX_FILES];
uint8_t expInternalIndex[MAX_FILES];
uint8_t expFileCount = 0;
uint8_t expFileIndex = 0;
uint8_t sdFileCount = 0;
uint8_t intFileIndex = 0;
char currentFile[13];
bool sdOk = false;
unsigned long lastSdCheckMs = 0;

struct InternalExp { const char *name; const char *const *lines; };
const char *const INT1_LINES[] = {
  "ID=INT1","PROGRAM=1","RETRIEVALS=0","INTERVAL_MIN=0","STEP_UNIT=SEC",
  "S1,3,0,1000,0000,0,0","S2,4,0,0100,0000,0,0","S3,5,0,0010,0000,0,0",
  "S4,3,0,0001,0000,0,0","S5,4,0,1100,0000,0,0", NULL
};
const char *const INT2_LINES[] = {
  "ID=INT2","PROGRAM=1","RETRIEVALS=0","INTERVAL_MIN=0","STEP_UNIT=SEC",
  "A1,5,0,1010,0000,0,0","A2,5,0,0101,0000,0,0","A3,5,0,0011,0000,0,0",
  "A4,5,0,1111,0000,0,0", NULL
};
const char *const INT3_LINES[] = {
  "ID=INT3","PROGRAM=1","RETRIEVALS=0","INTERVAL_MIN=0","STEP_UNIT=SEC",
  "B1,2,0,1000,0000,0,0","B2,2,0,0100,0000,0,0","B3,2,0,0010,0000,0,0",
  "B4,2,0,0001,0000,0,0","B5,2,0,1110,0000,0,0", NULL
};
const char *const INT4_LINES[] = {
  "ID=INT4","PROGRAM=1","RETRIEVALS=0","INTERVAL_MIN=0","STEP_UNIT=SEC",
  "T28,60,0,0000,0000,28,0", NULL
};
const InternalExp INTERNAL_EXPS[] = {
  {"INT1.CSV", INT1_LINES},
  {"INT2.CSV", INT2_LINES},
  {"INT3.CSV", INT3_LINES},
  {"INT4.CSV", INT4_LINES},
};
const uint8_t INTERNAL_COUNT = 4;

enum SdState { SD_UNAVAILABLE, SD_READY, SD_DEGRADED };
SdState sdState = SD_UNAVAILABLE;
unsigned long lastSdAttemptMs = 0;
const unsigned long SD_RETRY_MS = 5000;
bool sdDisconnectNotice = false;
bool sdReconnectNotice = false;
unsigned long noticeUntilMs = 0;
char noticeLine0[17] = "";
char noticeLine1[17] = "";

struct ThermoConfig {
  uint16_t minOnSec;
  uint16_t minOffSec;
  uint8_t heaterRelayBit;
  uint8_t mode;
  uint16_t safetyMaxSecOn;
};

enum NetState { NET_OFF, NET_CONNECTING, NET_ONLINE, NET_ERROR };

struct CloudConfig {
  uint8_t enabled;
  char deviceId[17];
  char ssid[33];
  char pass[65];
  char apiHost[48];
  char apiPath[24];  // base path, e.g. /v1
  char apiToken[40];
};

struct UploadCursor {
  char runFile[13];
  uint32_t byteOffset;
  uint32_t lineIndex;
  uint8_t synced;
};

struct NetStats {
  uint32_t sent;
  uint32_t failed;
  uint32_t retried;
  uint32_t pendingLines;
  int16_t lastHttpCode;
};

struct EventRecord {
  uint32_t tsMs;
  char eventType[12];
  int16_t arg0;
  int16_t arg1;
};

struct EepromConfigBlob {
  uint32_t signature;
  uint8_t version;
  ThermoConfig thermo;
  CloudConfig cloud;
  uint16_t checksum;
};

const uint32_t EEPROM_SIG = 0x5448524DUL; // THRM
const uint8_t EEPROM_VER = 2;
const int EEPROM_ADDR = 0;
ThermoConfig thermoCfg;
CloudConfig cloudCfg;
unsigned long heaterStateChangedMs = 0;
unsigned long heaterOnSinceMs = 0;
NetState netState = NET_OFF;
NetStats netStats = {};
UploadCursor uploadCursor = {};
UploadCursor eventCursor = {};
unsigned long lastNetAttemptMs = 0;
unsigned long lastCloudTickMs = 0;
unsigned long cloudBackoffMs = 1000;
const unsigned long CLOUD_TICK_MS = 3000;
const unsigned long CLOUD_CONNECT_RETRY_MS = 5000;
const uint8_t CLOUD_BATCH_MAX = 1;
const uint16_t CLOUD_JSON_MAX = 400;
char activeRunUpload[13] = "";
bool cloudBusy = false;
char cloudPayload[CLOUD_JSON_MAX];
char cloudPath[40];
uint16_t cloudPayloadLen = 0;
unsigned long cloudJobStartedMs = 0;
uint8_t cloudJobState = 0;
bool cloudJobIsEvent = false;
int cloudJobHttpCode = -1;
unsigned long cloudJobDeadlineMs = 0;
uint16_t cloudHttpLen = 0;
UploadCursor cloudNextCursor = {};
bool cloudHasCursorUpdate = false;
bool cloudLastJobDone = false;
bool cloudLastJobOk = false;
uint8_t wifiStage = 0;
unsigned long wifiStageMs = 0;
char espRxWindow[180];
uint16_t espRxLen = 0;
char serialCmdLine[120];
uint8_t serialCmdLen = 0;
uint32_t lastCloudSyncEpoch = 0;

struct RunState {
  bool active;
  bool paused;
  bool waitRetrieval;
  uint16_t stepCount;
  uint16_t currentStep;
  uint8_t retrievalIndex;
  unsigned long expStartMs;
  unsigned long totalPauseMs;
  unsigned long pausedAt;
} run = {};

File runFile;
File logFile;
bool logOpen = false;
enum RunSource { SRC_SD, SRC_INT };
RunSource currentSource = SRC_SD;
uint8_t currentInternalIndex = 0;
uint8_t currentInternalLine = 0;
const uint16_t MAX_STEPS = 90;
StepData stepCache[MAX_STEPS];
uint16_t stepCacheCount = 0;
uint16_t stepCacheIndex = 0;
bool stepCacheReady = false;

struct LogRecord {
  uint32_t ms;
  int16_t t1_10;
  int16_t h1_10;
  int16_t t2_10;
  int16_t h2_10;
  int16_t tAvg_10;
  int16_t hAvg_10;
  uint8_t mask;
  char step[10];
};

const uint8_t LOG_BACKLOG_CAP = 8;
LogRecord logQueue[LOG_BACKLOG_CAP];
uint8_t logHead = 0;
uint8_t logTail = 0;
uint8_t logCount = 0;
uint16_t droppedLogsCount = 0;
unsigned long lastFlushTryMs = 0;

RTC_DS1307 rtc;
bool rtcOk = false;
bool rtcLostPowerOrInvalid = false;

struct TimeSetState {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t field;
};
TimeSetState timeSet = {2026, 1, 1, 0, 0, 0, 0};

// Step timing
bool stepActive = false;
bool stepDone = false;
unsigned long stepStartMs = 0;
unsigned long stepDurationMs = 0;

// Sensors
const unsigned long DHT_PERIOD_MS = 3000;
const unsigned long DHT_FAIL_RETRY_MS = 2000;
const unsigned long STEP_UNIT_MS_DEFAULT = 1000UL; // seconds-based steps
const unsigned long SD_CHECK_MS = 3000;
const unsigned long LOG_PERIOD_MS = 3000;
const unsigned long LOG_FLUSH_INTERVAL_MS = 400;
const uint8_t LOG_FLUSH_EVERY = 5;
const uint8_t LOG_FLUSH_BURST = 3;
unsigned long lastReadMs = 0;
unsigned long lastLogMs = 0;
uint8_t logFlushCounter = 0;
bool haveValid = false;
float t1 = NAN, h1 = NAN, t2 = NAN, h2 = NAN;
float tAvg = NAN, hAvg = NAN;
bool dht1Ok = false;
bool dht2Ok = false;
unsigned long lastValidSensorMs = 0;

// Relay + thermostat
uint8_t relayMask = 0;
bool heaterOn = false;

// Timing
const unsigned long POLL_MS = 3000;
unsigned long lastPoll = 0;

// ===== Utility =====
void print16(int col, int row, const char *s) {
  lcd.setCursor(col, row);
  for (int i = 0; i < 16; i++) {
    char c = (s && s[i]) ? s[i] : ' ';
    lcd.print(c);
  }
}

void safeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

int cmpIgnoreCase(const char *a, const char *b) {
  while (*a && *b) {
    char ca = tolower(*a++);
    char cb = tolower(*b++);
    if (ca != cb) return ca - cb;
  }
  return tolower(*a) - tolower(*b);
}

uint8_t maskFromString(const char *s) {
  if (!s) return 0;
  uint8_t m = 0;
  for (uint8_t i = 0; s[i] && i < 4; i++) if (s[i] == '1') m |= (1 << i);
  return m;
}

void maskToChars(uint8_t mask, char *out) {
  out[0] = (mask & 0x01) ? '1' : '0';
  out[1] = (mask & 0x02) ? '1' : '0';
  out[2] = (mask & 0x04) ? '1' : '0';
  out[3] = (mask & 0x08) ? '1' : '0';
  out[4] = '\0';
}

void fmtFloat1(char *out, float v) {
  dtostrf(v, 5, 1, out);
  // trim leading space
  if (out[0] == ' ') {
    memmove(out, out + 1, strlen(out));
  }
}

char* trimInPlace(char *s) {
  if (!s) return s;
  while (*s && isspace(*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace(*(e - 1))) *(--e) = '\0';
  return s;
}

bool isLeapYear(uint16_t y) {
  if ((y % 400U) == 0U) return true;
  if ((y % 100U) == 0U) return false;
  return (y % 4U) == 0U;
}

uint8_t daysInMonth(uint16_t y, uint8_t m) {
  static const uint8_t dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m < 1 || m > 12) return 31;
  if (m == 2 && isLeapYear(y)) return 29;
  return dm[m - 1];
}

void clampTimeSet() {
  if (timeSet.year < 2000) timeSet.year = 2000;
  if (timeSet.year > 2099) timeSet.year = 2099;
  if (timeSet.month < 1) timeSet.month = 1;
  if (timeSet.month > 12) timeSet.month = 12;
  uint8_t dim = daysInMonth(timeSet.year, timeSet.month);
  if (timeSet.day < 1) timeSet.day = 1;
  if (timeSet.day > dim) timeSet.day = dim;
  if (timeSet.hour > 23) timeSet.hour = 23;
  if (timeSet.minute > 59) timeSet.minute = 59;
  if (timeSet.second > 59) timeSet.second = 59;
  if (timeSet.field > 5) timeSet.field = 5;
}

void loadTimeSetFromRtc() {
  if (rtcOk) {
    DateTime now = rtc.now();
    timeSet.year = (uint16_t)now.year();
    timeSet.month = (uint8_t)now.month();
    timeSet.day = (uint8_t)now.day();
    timeSet.hour = (uint8_t)now.hour();
    timeSet.minute = (uint8_t)now.minute();
    timeSet.second = (uint8_t)now.second();
  } else {
    timeSet.year = 2026;
    timeSet.month = 1;
    timeSet.day = 1;
    timeSet.hour = 0;
    timeSet.minute = 0;
    timeSet.second = 0;
  }
  timeSet.field = 0;
  clampTimeSet();
}

void adjustTimeField(int8_t delta) {
  if (timeSet.field == 0) {
    int v = (int)timeSet.year + delta;
    if (v < 2000) v = 2099;
    if (v > 2099) v = 2000;
    timeSet.year = (uint16_t)v;
  } else if (timeSet.field == 1) {
    int v = (int)timeSet.month + delta;
    if (v < 1) v = 12;
    if (v > 12) v = 1;
    timeSet.month = (uint8_t)v;
  } else if (timeSet.field == 2) {
    uint8_t dim = daysInMonth(timeSet.year, timeSet.month);
    int v = (int)timeSet.day + delta;
    if (v < 1) v = dim;
    if (v > dim) v = 1;
    timeSet.day = (uint8_t)v;
  } else if (timeSet.field == 3) {
    int v = (int)timeSet.hour + delta;
    if (v < 0) v = 23;
    if (v > 23) v = 0;
    timeSet.hour = (uint8_t)v;
  } else if (timeSet.field == 4) {
    int v = (int)timeSet.minute + delta;
    if (v < 0) v = 59;
    if (v > 59) v = 0;
    timeSet.minute = (uint8_t)v;
  } else if (timeSet.field == 5) {
    int v = (int)timeSet.second + delta;
    if (v < 0) v = 59;
    if (v > 59) v = 0;
    timeSet.second = (uint8_t)v;
  }
  clampTimeSet();
}

bool saveTimeSetToRtc() {
  if (!rtcOk) return false;
  clampTimeSet();
  DateTime dt(timeSet.year, timeSet.month, timeSet.day, timeSet.hour, timeSet.minute, timeSet.second);
  rtc.adjust(dt);
  rtcLostPowerOrInvalid = false;
  return true;
}

void showNotice(const char *l0, const char *l1, unsigned long ms) {
  safeCopy(noticeLine0, sizeof(noticeLine0), l0 ? l0 : "");
  safeCopy(noticeLine1, sizeof(noticeLine1), l1 ? l1 : "");
  noticeUntilMs = millis() + ms;
}

bool noticeActive() {
  return noticeUntilMs > millis();
}

uint16_t cfgChecksum(const EepromConfigBlob &b) {
  const uint8_t *p = (const uint8_t*)&b;
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(EepromConfigBlob) - sizeof(uint16_t); i++) sum = (uint16_t)(sum + p[i]);
  return sum;
}

void clearEspRxWindow() {
  espRxLen = 0;
  espRxWindow[0] = '\0';
}

void appendEspRx(char c) {
  if (espRxLen + 1 < sizeof(espRxWindow)) {
    espRxWindow[espRxLen++] = c;
  } else {
    memmove(espRxWindow, espRxWindow + 1, sizeof(espRxWindow) - 2);
    espRxLen = sizeof(espRxWindow) - 2;
    espRxWindow[espRxLen++] = c;
  }
  espRxWindow[espRxLen] = '\0';
}

bool espHas(const char *pat) {
  return strstr(espRxWindow, pat) != NULL;
}

void espSendCmd(const char *cmd) {
  clearEspRxWindow();
  Serial1.print(cmd);
  Serial1.print("\r\n");
}

const char* netStateTxt() {
  if (netState == NET_OFF) return "OFF";
  if (netState == NET_CONNECTING) return "CON";
  if (netState == NET_ONLINE) return "ON ";
  return "ERR";
}

void setDefaultThermoConfig() {
  thermoCfg.minOnSec = 10;
  thermoCfg.minOffSec = 10;
  thermoCfg.heaterRelayBit = 2;
  thermoCfg.mode = 0;
  thermoCfg.safetyMaxSecOn = 180;
}

void setDefaultCloudConfig() {
  memset(&cloudCfg, 0, sizeof(cloudCfg));
  cloudCfg.enabled = 0;
  safeCopy(cloudCfg.deviceId, sizeof(cloudCfg.deviceId), "MEGA001");
  safeCopy(cloudCfg.apiPath, sizeof(cloudCfg.apiPath), "/v1");
}

bool cloudConfigValid() {
  if (!cloudCfg.enabled) return true;
  if (cloudCfg.ssid[0] == '\0') return false;
  if (cloudCfg.apiHost[0] == '\0') return false;
  if (cloudCfg.apiPath[0] == '\0') return false;
  if (cloudCfg.apiToken[0] == '\0') return false;
  return true;
}

void saveConfigToEeprom() {
  EepromConfigBlob blob;
  blob.signature = EEPROM_SIG;
  blob.version = EEPROM_VER;
  blob.thermo = thermoCfg;
  blob.cloud = cloudCfg;
  blob.checksum = 0;
  blob.checksum = cfgChecksum(blob);
  EEPROM.put(EEPROM_ADDR, blob);
}

struct LegacyEepromConfigBlob {
  uint32_t signature;
  uint8_t version;
  ThermoConfig thermo;
  uint16_t checksum;
};

uint16_t cfgChecksumLegacy(const LegacyEepromConfigBlob &b) {
  const uint8_t *p = (const uint8_t*)&b;
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(LegacyEepromConfigBlob) - sizeof(uint16_t); i++) sum = (uint16_t)(sum + p[i]);
  return sum;
}

bool loadConfigFromEeprom() {
  EepromConfigBlob blob;
  EEPROM.get(EEPROM_ADDR, blob);
  if (blob.signature == EEPROM_SIG && blob.version == EEPROM_VER && blob.checksum == cfgChecksum(blob)) {
    thermoCfg = blob.thermo;
    cloudCfg = blob.cloud;
  } else {
    // Migration path from old EEPROM v1 (thermo only)
    LegacyEepromConfigBlob oldBlob;
    EEPROM.get(EEPROM_ADDR, oldBlob);
    if (oldBlob.signature != EEPROM_SIG || oldBlob.version != 1 || oldBlob.checksum != cfgChecksumLegacy(oldBlob)) {
      return false;
    }
    thermoCfg = oldBlob.thermo;
    setDefaultCloudConfig();
  }
  if (thermoCfg.minOnSec == 0 || thermoCfg.minOnSec > 600) return false;
  if (thermoCfg.minOffSec == 0 || thermoCfg.minOffSec > 600) return false;
  if (thermoCfg.heaterRelayBit > 3) return false;
  if (thermoCfg.safetyMaxSecOn > 3600) return false;
  if (cloudCfg.deviceId[0] == '\0') safeCopy(cloudCfg.deviceId, sizeof(cloudCfg.deviceId), "MEGA001");
  if (cloudCfg.apiPath[0] == '\0') safeCopy(cloudCfg.apiPath, sizeof(cloudCfg.apiPath), "/v1");
  if (cloudCfg.enabled > 1) cloudCfg.enabled = 0;
  return true;
}

void saveThermoToEeprom() { saveConfigToEeprom(); }
bool loadThermoFromEeprom() { return loadConfigFromEeprom(); }

// ===== SD =====
bool initSD() {
  // Mega requires SS (53) as OUTPUT to keep SPI master mode
  pinMode(53, OUTPUT);
  digitalWrite(53, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  return SD.begin(SD_CS);
}

void setSdState(SdState st) {
  if (sdState == st) return;
  SdState prev = sdState;
  sdState = st;
  sdOk = (sdState == SD_READY);
  if (prev == SD_READY && sdState != SD_READY && run.active) {
    sdDisconnectNotice = true;
  }
  if (prev != SD_READY && sdState == SD_READY && run.active) {
    sdReconnectNotice = true;
  }
}

bool ensureSdReady(bool force) {
  unsigned long now = millis();
  unsigned long waitMs = force ? 0 : SD_RETRY_MS;
  if (!force && sdState == SD_READY) return true;
  if (lastSdAttemptMs != 0 && now - lastSdAttemptMs < waitMs) return sdState == SD_READY;

  lastSdAttemptMs = now;
  if (!initSD()) {
    setSdState(run.active ? SD_DEGRADED : SD_UNAVAILABLE);
    return false;
  }
  File root = SD.open("/");
  if (!root) {
    setSdState(run.active ? SD_DEGRADED : SD_UNAVAILABLE);
    return false;
  }
  root.close();
  setSdState(SD_READY);
  return true;
}

bool checkSD() {
  unsigned long now = millis();
  if (now - lastSdCheckMs < SD_CHECK_MS) return sdState == SD_READY;
  lastSdCheckMs = now;
  return ensureSdReady(false);
}

void addInternalExperiments() {
  for (uint8_t i = 0; i < INTERNAL_COUNT && expFileCount < MAX_FILES; i++) {
    safeCopy(expFiles[expFileCount], sizeof(expFiles[expFileCount]), INTERNAL_EXPS[i].name);
    expIsInternal[expFileCount] = true;
    expInternalIndex[expFileCount] = i;
    expFileCount++;
  }
}

bool hasCsvExt(const char *name) {
  if (!name) return false;
  size_t l = strlen(name);
  return l >= 4 && tolower(name[l-4])=='.' && tolower(name[l-3])=='c' && tolower(name[l-2])=='s' && tolower(name[l-1])=='v';
}

void scanExperimentFiles() {
  expFileCount = 0;
  if (checkSD()) {
    File root = SD.open("/");
    if (root) {
      root.rewindDirectory();
      while (true) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
          const char *nm = f.name();
          if (hasCsvExt(nm) && expFileCount < MAX_FILES) {
            safeCopy(expFiles[expFileCount], sizeof(expFiles[expFileCount]), nm);
            expIsInternal[expFileCount] = false;
            expInternalIndex[expFileCount] = 0;
            expFileCount++;
          }
        }
        f.close();
      }
      root.close();
    } else {
      setSdState(run.active ? SD_DEGRADED : SD_UNAVAILABLE);
    }
  }
  sdFileCount = expFileCount;
  addInternalExperiments();
}

uint16_t parseUint(const char *s, uint16_t def) { if (!s||!*s) return def; return (uint16_t)strtoul(s, NULL, 10); }
float parseFloat(const char *s, float def) { if (!s||!*s) return def; return atof(s); }

uint16_t parseStepUnitMs(const char *v) {
  if (!v || !*v) return (uint16_t)STEP_UNIT_MS_DEFAULT;
  if (cmpIgnoreCase(v, "S") == 0 || cmpIgnoreCase(v, "SEC") == 0 || cmpIgnoreCase(v, "SEG") == 0) return 1000;
  if (cmpIgnoreCase(v, "M") == 0 || cmpIgnoreCase(v, "MIN") == 0) return 60000;
  uint16_t ms = (uint16_t)parseUint(v, (uint16_t)STEP_UNIT_MS_DEFAULT);
  if (ms == 0) ms = (uint16_t)STEP_UNIT_MS_DEFAULT;
  return ms;
}

void applyThermoOverride(const char *key, const char *val) {
  uint16_t v = parseUint(val, 0);
  if (cmpIgnoreCase(key, "THERMO_MIN_ON_S") == 0) {
    if (v >= 1 && v <= 600) thermoCfg.minOnSec = v;
  } else if (cmpIgnoreCase(key, "THERMO_MIN_OFF_S") == 0) {
    if (v >= 1 && v <= 600) thermoCfg.minOffSec = v;
  } else if (cmpIgnoreCase(key, "THERMO_TOGGLE_S") == 0) {
    if (v >= 1 && v <= 600) {
      thermoCfg.minOnSec = v;
      thermoCfg.minOffSec = v;
    }
  } else if (cmpIgnoreCase(key, "THERMO_HEATER_BIT") == 0) {
    if (v <= 3) thermoCfg.heaterRelayBit = (uint8_t)v;
  } else if (cmpIgnoreCase(key, "THERMO_SAFETY_MAX_ON_S") == 0) {
    if (v <= 3600) thermoCfg.safetyMaxSecOn = v;
  } else if (cmpIgnoreCase(key, "WIFI_ENABLE") == 0) {
    cloudCfg.enabled = (v ? 1 : 0);
  } else if (cmpIgnoreCase(key, "WIFI_SSID") == 0) {
    safeCopy(cloudCfg.ssid, sizeof(cloudCfg.ssid), val);
  } else if (cmpIgnoreCase(key, "WIFI_PASS") == 0) {
    safeCopy(cloudCfg.pass, sizeof(cloudCfg.pass), val);
  } else if (cmpIgnoreCase(key, "API_HOST") == 0) {
    safeCopy(cloudCfg.apiHost, sizeof(cloudCfg.apiHost), val);
  } else if (cmpIgnoreCase(key, "API_PATH") == 0) {
    safeCopy(cloudCfg.apiPath, sizeof(cloudCfg.apiPath), val);
  } else if (cmpIgnoreCase(key, "API_TOKEN") == 0) {
    safeCopy(cloudCfg.apiToken, sizeof(cloudCfg.apiToken), val);
  } else if (cmpIgnoreCase(key, "DEVICE_ID") == 0) {
    safeCopy(cloudCfg.deviceId, sizeof(cloudCfg.deviceId), val);
  }
}

bool loadConfigOverridesFromSD() {
  if (!ensureSdReady(false)) return false;
  File cfg = SD.open("CONFIG.CSV", FILE_READ);
  if (!cfg) return false;
  char line[128];
  while (cfg.available()) {
    size_t n = cfg.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
    if (n == 0 || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *k = trimInPlace(line);
    char *v = trimInPlace(eq + 1);
    if (!k || !*k || !v || !*v) continue;
    applyThermoOverride(k, v);
  }
  cfg.close();
  return true;
}

void loadThermoConfigChain() {
  setDefaultThermoConfig();
  setDefaultCloudConfig();
  if (!loadThermoFromEeprom()) saveThermoToEeprom();
  loadConfigOverridesFromSD();
  if (!cloudConfigValid()) cloudCfg.enabled = 0;
}

void parseMetaLine(char *line) {
  char *eq = strchr(line, '=');
  if (eq) {
    *eq = '\0';
    const char *k = line; const char *v = eq + 1;
    if (cmpIgnoreCase(k, "ID") == 0) safeCopy(meta.id, sizeof(meta.id), v);
    else if (cmpIgnoreCase(k, "PROGRAM") == 0) meta.program = (uint8_t)parseUint(v, 1);
    else if (cmpIgnoreCase(k, "RETRIEVALS") == 0 || cmpIgnoreCase(k, "RETIRADAS") == 0) meta.retrievals = (uint8_t)parseUint(v, 0);
    else if (cmpIgnoreCase(k, "INTERVAL_MIN") == 0 || cmpIgnoreCase(k, "INTERVALO") == 0) meta.intervalMin = parseUint(v, 0);
    else if (cmpIgnoreCase(k, "STEP_UNIT") == 0 || cmpIgnoreCase(k, "STEP_UNIT_MS") == 0 || cmpIgnoreCase(k, "UNIDADE") == 0) meta.stepUnitMs = parseStepUnitMs(v);
    return;
  }
  char *first = strtok(line, ",");
  char *second = strtok(NULL, ",");
  if (!first || !second) return;
  if (cmpIgnoreCase(first, "ID") == 0) safeCopy(meta.id, sizeof(meta.id), second);
  else if (cmpIgnoreCase(first, "PROGRAM") == 0) meta.program = (uint8_t)parseUint(second, 1);
  else if (cmpIgnoreCase(first, "RETRIEVALS") == 0 || cmpIgnoreCase(first, "RETIRADAS") == 0) meta.retrievals = (uint8_t)parseUint(second, 0);
  else if (cmpIgnoreCase(first, "INTERVAL_MIN") == 0 || cmpIgnoreCase(first, "INTERVALO") == 0) meta.intervalMin = parseUint(second, 0);
  else if (cmpIgnoreCase(first, "STEP_UNIT") == 0 || cmpIgnoreCase(first, "STEP_UNIT_MS") == 0 || cmpIgnoreCase(first, "UNIDADE") == 0) meta.stepUnitMs = parseStepUnitMs(second);
}

bool parseStepLine(char *line, StepData &out) {
  char *tok[8] = {0};
  uint8_t n = 0;
  char *p = strtok(line, ",");
  while (p && n < 8) { tok[n++] = p; p = strtok(NULL, ","); }
  if (n < 5) return false;
  safeCopy(out.label, sizeof(out.label), tok[0]);
  out.seconds = parseUint(tok[1], 0);
  out.mask = maskFromString(tok[3]);
  out.tmin10 = 0;
  out.tmax10 = 0;
  if (n >= 6 && tok[5]) out.tmin10 = (uint16_t)(parseFloat(tok[5], 0) * 10.0f);
  if (n >= 7 && tok[6]) out.tmax10 = (uint16_t)(parseFloat(tok[6], 0) * 10.0f);
  return true;
}

void resetStepCache() {
  stepCacheCount = 0;
  stepCacheIndex = 0;
  stepCacheReady = false;
}

bool loadExperiment(const char *fileName) {
  if (!hasCsvExt(fileName)) return false;
  if (!ensureSdReady(false)) return false;
  File f = SD.open(fileName, FILE_READ);
  if (!f) return false;
  meta = {};
  meta.program = 1;
  meta.stepCount = 0;
  meta.stepUnitMs = (uint16_t)STEP_UNIT_MS_DEFAULT;
  resetStepCache();
  char line[96];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n && (line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = '\0';
    if (n == 0 || line[0] == '#') continue;
    char tmp[96]; strncpy(tmp, line, sizeof(tmp) - 1); tmp[95] = '\0';
    bool isMeta = false;
    if (strchr(tmp, '=')) isMeta = true;
    if (strncmp(tmp, "ID", 2) == 0 || strncmp(tmp, "PROGRAM", 7) == 0 || strncmp(tmp, "RETRIEVAL", 9) == 0 ||
        strncmp(tmp, "RETIRADAS", 9) == 0 || strncmp(tmp, "INTERVAL", 8) == 0) isMeta = true;
    if (isMeta) parseMetaLine(tmp);
    else {
      StepData st;
      if (parseStepLine(tmp, st)) {
        if (stepCacheCount >= MAX_STEPS) { f.close(); return false; }
        stepCache[stepCacheCount++] = st;
      }
    }
  }
  f.close();
  safeCopy(currentFile, sizeof(currentFile), fileName);
  currentSource = SRC_SD;
  meta.stepCount = stepCacheCount;
  stepCacheReady = (stepCacheCount > 0);
  return stepCacheReady;
}

bool loadExperimentInternal(uint8_t idx) {
  if (idx >= INTERNAL_COUNT) return false;
  meta = {};
  meta.program = 1;
  meta.stepCount = 0;
  meta.stepUnitMs = (uint16_t)STEP_UNIT_MS_DEFAULT;
  resetStepCache();
  const char *const *lines = INTERNAL_EXPS[idx].lines;
  for (uint16_t i = 0; lines[i]; i++) {
    char tmp[96];
    strncpy(tmp, lines[i], sizeof(tmp) - 1);
    tmp[95] = '\0';
    if (tmp[0] == '\0' || tmp[0] == '#') continue;
    bool isMeta = false;
    if (strchr(tmp, '=')) isMeta = true;
    if (strncmp(tmp, "ID", 2) == 0 || strncmp(tmp, "PROGRAM", 7) == 0 || strncmp(tmp, "RETRIEVAL", 9) == 0 ||
        strncmp(tmp, "RETIRADAS", 9) == 0 || strncmp(tmp, "INTERVAL", 8) == 0 || strncmp(tmp, "STEP_UNIT", 9) == 0 ||
        strncmp(tmp, "UNIDADE", 7) == 0) isMeta = true;
    if (isMeta) parseMetaLine(tmp);
    else {
      StepData st;
      if (parseStepLine(tmp, st)) {
        if (stepCacheCount >= MAX_STEPS) return false;
        stepCache[stepCacheCount++] = st;
      }
    }
  }
  safeCopy(currentFile, sizeof(currentFile), INTERNAL_EXPS[idx].name);
  currentSource = SRC_INT;
  currentInternalIndex = idx;
  meta.stepCount = stepCacheCount;
  stepCacheReady = (stepCacheCount > 0);
  return stepCacheReady;
}

bool openRunFile() {
  if (runFile) runFile.close();
  currentInternalLine = 0;
  stepCacheIndex = 0;
  return stepCacheReady;
}

bool openLogFile() {
  if (!ensureSdReady(false)) return false;
  for (uint8_t i = 1; i < 99; i++) {
    char name[13];
    snprintf(name, sizeof(name), "RUN%02u.CSV", i);
    if (!SD.exists(name)) {
      logFile = SD.open(name, FILE_WRITE);
      if (!logFile) {
        setSdState(run.active ? SD_DEGRADED : SD_UNAVAILABLE);
        return false;
      }
      logFile.println("ms;T1;U1;T2;U2;Tavg;Uavg;mask;step");
      logOpen = true;
      return true;
    }
  }
  return false;
}

bool readNextStep(StepData &st) {
  if (!stepCacheReady || stepCacheIndex >= stepCacheCount) return false;
  st = stepCache[stepCacheIndex++];
  return true;
}

void resetLogQueue() {
  logHead = 0;
  logTail = 0;
  logCount = 0;
  droppedLogsCount = 0;
}

void queueLogRecord(const LogRecord &rec) {
  if (logCount >= LOG_BACKLOG_CAP) {
    logHead = (uint8_t)((logHead + 1) % LOG_BACKLOG_CAP);
    logCount--;
    droppedLogsCount++;
  }
  logQueue[logTail] = rec;
  logTail = (uint8_t)((logTail + 1) % LOG_BACKLOG_CAP);
  logCount++;
}

bool popLogRecord(LogRecord &out) {
  if (logCount == 0) return false;
  out = logQueue[logHead];
  logHead = (uint8_t)((logHead + 1) % LOG_BACKLOG_CAP);
  logCount--;
  return true;
}

void printScaled10(int16_t val) {
  bool neg = val < 0;
  uint16_t a = (uint16_t)(neg ? -val : val);
  if (neg) logFile.print('-');
  logFile.print((int)(a / 10));
  logFile.print('.');
  logFile.print((int)(a % 10));
}

bool writeLogRecord(const LogRecord &rec) {
  if (!logOpen || !logFile) return false;
  logFile.print(rec.ms);
  logFile.print(';');
  printScaled10(rec.t1_10);
  logFile.print(';');
  printScaled10(rec.h1_10);
  logFile.print(';');
  printScaled10(rec.t2_10);
  logFile.print(';');
  printScaled10(rec.h2_10);
  logFile.print(';');
  printScaled10(rec.tAvg_10);
  logFile.print(';');
  printScaled10(rec.hAvg_10);
  logFile.print(';');
  logFile.print(rec.mask);
  logFile.print(';');
  logFile.println(rec.step);
  return (bool)logFile;
}

void processLogFlush() {
  if (logCount == 0) return;
  unsigned long now = millis();
  if (now - lastFlushTryMs < LOG_FLUSH_INTERVAL_MS) return;
  lastFlushTryMs = now;

  if (!logOpen) {
    if (!openLogFile()) return;
  }

  for (uint8_t i = 0; i < LOG_FLUSH_BURST && logCount > 0; i++) {
    LogRecord rec;
    if (!popLogRecord(rec)) break;
    if (!writeLogRecord(rec)) {
      queueLogRecord(rec);
      if (logOpen) { logFile.close(); logOpen = false; }
      setSdState(run.active ? SD_DEGRADED : SD_UNAVAILABLE);
      return;
    }
  }

  if (++logFlushCounter >= LOG_FLUSH_EVERY) {
    logFile.flush();
    logFlushCounter = 0;
  }
}

struct TelemetryRow {
  uint32_t lineIndex;
  uint32_t ms;
  char t1[10];
  char u1[10];
  char t2[10];
  char u2[10];
  char tavg[10];
  char uavg[10];
  uint8_t mask;
  char step[10];
};

struct EventUploadRow {
  uint32_t lineIndex;
  uint32_t ms;
  char rtcIso[24];
  char eventType[12];
  char screenName[14];
  int16_t arg0;
  int16_t arg1;
  char runFile[13];
  uint16_t step;
};

bool appendFmt(char *buf, size_t cap, size_t &len, const char *fmt, ...) {
  if (len >= cap) return false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0) return false;
  if ((size_t)n >= cap - len) return false;
  len += (size_t)n;
  return true;
}

void getRtcIso(char *out, size_t outSize) {
  if (!out || outSize == 0) return;
  if (rtcOk) {
    DateTime dt = rtc.now();
    snprintf(out, outSize, "%04u-%02u-%02uT%02u:%02u:%02uZ",
      (unsigned)dt.year(), (unsigned)dt.month(), (unsigned)dt.day(),
      (unsigned)dt.hour(), (unsigned)dt.minute(), (unsigned)dt.second());
  } else {
    snprintf(out, outSize, "1970-01-01T00:00:00Z");
  }
}

const char* sdStateTxt() {
  if (sdState == SD_READY) return "ok";
  if (sdState == SD_DEGRADED) return "degraded";
  return "unavailable";
}

const char* rtcStateTxt() {
  if (!rtcOk) return "fail";
  return rtcLostPowerOrInvalid ? "invalid" : "ok";
}

const char* runStateTxt() {
  if (!run.active) return "idle";
  if (run.paused) return "paused";
  return "running";
}

const char* screenName(UiScreen s) {
  switch (s) {
    case SCREEN_MENU: return "menu";
    case SCREEN_EXP_LIST: return "exp_list";
    case SCREEN_INT_LIST: return "int_list";
    case SCREEN_SERVICE_MENU: return "service";
    case SCREEN_CONFIG_MENU: return "cfg";
    case SCREEN_CONFIG_HEATER_INTERVAL: return "cfg_int";
    case SCREEN_TIME_SET: return "time_set";
    case SCREEN_SENSOR_TEST: return "sensor";
    case SCREEN_RELAY_TEST: return "relay";
    case SCREEN_WIFI_STATUS: return "wifi";
    case SCREEN_RUNNING: return "running";
    case SCREEN_CONFIRM_STOP: return "confirm";
    case SCREEN_RETRIEVAL: return "retrieval";
    default: return "unknown";
  }
}

bool isRunCsvFile(const char *name) {
  if (!name || !hasCsvExt(name)) return false;
  if (strlen(name) < 7) return false;
  return toupper(name[0]) == 'R' && toupper(name[1]) == 'U' && toupper(name[2]) == 'N';
}

void ackNameFromCsv(const char *csvName, char *ackName, size_t ackSize) {
  safeCopy(ackName, ackSize, csvName);
  char *dot = strrchr(ackName, '.');
  if (dot) {
    safeCopy(dot, (size_t)(ackName + ackSize - dot), ".ACK");
  } else {
    safeCopy(ackName, ackSize, "SYNC.ACK");
  }
}

bool syncIndexLoad(const char *csvName, UploadCursor &cursor) {
  memset(&cursor, 0, sizeof(cursor));
  safeCopy(cursor.runFile, sizeof(cursor.runFile), csvName);
  char ackName[13];
  ackNameFromCsv(csvName, ackName, sizeof(ackName));
  if (!ensureSdReady(false)) return false;
  File ack = SD.open(ackName, FILE_READ);
  if (!ack) return true;
  char line[48];
  size_t n = ack.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = '\0';
  ack.close();
  char *a = strtok(line, ",");
  char *b = strtok(NULL, ",");
  char *c = strtok(NULL, ",");
  if (a) cursor.byteOffset = strtoul(a, NULL, 10);
  if (b) cursor.lineIndex = strtoul(b, NULL, 10);
  if (c) lastCloudSyncEpoch = strtoul(c, NULL, 10);
  return true;
}

bool syncIndexSave(const UploadCursor &cursor) {
  if (!ensureSdReady(false)) return false;
  char ackName[13];
  ackNameFromCsv(cursor.runFile, ackName, sizeof(ackName));
  if (SD.exists(ackName)) SD.remove(ackName);
  File ack = SD.open(ackName, FILE_WRITE);
  if (!ack) return false;
  ack.print(cursor.byteOffset);
  ack.print(',');
  ack.print(cursor.lineIndex);
  ack.print(',');
  ack.println(lastCloudSyncEpoch);
  ack.close();
  return true;
}

bool parseTelemetryLine(const char *line, TelemetryRow &row) {
  char tmp[96];
  safeCopy(tmp, sizeof(tmp), line);
  char *tok[9] = {0};
  uint8_t n = 0;
  char *p = strtok(tmp, ";");
  while (p && n < 9) { tok[n++] = p; p = strtok(NULL, ";"); }
  if (n < 9) return false;
  if (!isdigit(tok[0][0])) return false;
  row.ms = strtoul(tok[0], NULL, 10);
  safeCopy(row.t1, sizeof(row.t1), tok[1]);
  safeCopy(row.u1, sizeof(row.u1), tok[2]);
  safeCopy(row.t2, sizeof(row.t2), tok[3]);
  safeCopy(row.u2, sizeof(row.u2), tok[4]);
  safeCopy(row.tavg, sizeof(row.tavg), tok[5]);
  safeCopy(row.uavg, sizeof(row.uavg), tok[6]);
  row.mask = (uint8_t)parseUint(tok[7], 0);
  safeCopy(row.step, sizeof(row.step), tok[8]);
  return true;
}

bool readTelemetryBatch(const char *runName, const UploadCursor &from, TelemetryRow *rows, uint8_t maxRows, UploadCursor &to, uint8_t &count) {
  count = 0;
  to = from;
  if (!ensureSdReady(false)) return false;
  File f = SD.open(runName, FILE_READ);
  if (!f) return false;
  if (!f.seek(from.byteOffset)) {
    f.close();
    return false;
  }
  uint32_t offset = from.byteOffset;
  uint32_t lineIndex = from.lineIndex;
  char line[96];
  while (f.available() && count < maxRows) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
    offset = f.position();
    if (n == 0) continue;
    if (strncmp(line, "ms;", 3) == 0) continue;
    TelemetryRow r;
    if (!parseTelemetryLine(line, r)) continue;
    lineIndex++;
    r.lineIndex = lineIndex;
    rows[count++] = r;
  }
  uint32_t sizeNow = f.size();
  f.close();
  to.byteOffset = offset;
  to.lineIndex = lineIndex;
  to.synced = (offset >= sizeNow) ? 1 : 0;
  return true;
}

bool parseEventLine(const char *line, EventUploadRow &row) {
  char tmp[128];
  safeCopy(tmp, sizeof(tmp), line);
  char *tok[8] = {0};
  uint8_t n = 0;
  char *p = strtok(tmp, ";");
  while (p && n < 8) { tok[n++] = p; p = strtok(NULL, ";"); }
  if (n < 8) return false;
  if (!isdigit(tok[0][0])) return false;
  row.ms = strtoul(tok[0], NULL, 10);
  safeCopy(row.rtcIso, sizeof(row.rtcIso), tok[1]);
  safeCopy(row.eventType, sizeof(row.eventType), tok[2]);
  safeCopy(row.screenName, sizeof(row.screenName), tok[3]);
  row.arg0 = (int16_t)atoi(tok[4]);
  row.arg1 = (int16_t)atoi(tok[5]);
  safeCopy(row.runFile, sizeof(row.runFile), tok[6]);
  row.step = (uint16_t)atoi(tok[7]);
  return true;
}

bool readEventBatch(const UploadCursor &from, EventUploadRow *rows, uint8_t maxRows, UploadCursor &to, uint8_t &count) {
  count = 0;
  to = from;
  if (!ensureSdReady(false)) return false;
  File f = SD.open("EVENTS.CSV", FILE_READ);
  if (!f) return false;
  if (!f.seek(from.byteOffset)) {
    f.close();
    return false;
  }
  uint32_t offset = from.byteOffset;
  uint32_t lineIndex = from.lineIndex;
  char line[128];
  while (f.available() && count < maxRows) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
    offset = f.position();
    if (n == 0) continue;
    if (strncmp(line, "ms;", 3) == 0) continue;
    EventUploadRow r;
    if (!parseEventLine(line, r)) continue;
    lineIndex++;
    r.lineIndex = lineIndex;
    rows[count++] = r;
  }
  uint32_t sizeNow = f.size();
  f.close();
  to.byteOffset = offset;
  to.lineIndex = lineIndex;
  to.synced = (offset >= sizeNow) ? 1 : 0;
  return true;
}

bool findPendingRunForUpload(char *runNameOut, UploadCursor &cursorOut) {
  if (!ensureSdReady(false)) return false;
  File root = SD.open("/");
  if (!root) return false;
  bool found = false;
  char candidate[13] = "";
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      const char *nm = f.name();
      if (isRunCsvFile(nm)) {
        char runName[13];
        safeCopy(runName, sizeof(runName), nm);
        uint32_t fileSize = f.size();
        UploadCursor c;
        syncIndexLoad(runName, c);
        if (c.byteOffset < fileSize) {
          if (!found || cmpIgnoreCase(runName, candidate) < 0) {
            safeCopy(candidate, sizeof(candidate), runName);
            cursorOut = c;
            found = true;
          }
        }
      }
    }
    f.close();
  }
  root.close();
  if (!found) return false;
  safeCopy(runNameOut, 13, candidate);
  return true;
}

void makeEndpointPath(bool isEvent, char *out, size_t outSize) {
  char base[40];
  safeCopy(base, sizeof(base), cloudCfg.apiPath);
  if (base[0] == '\0') safeCopy(base, sizeof(base), "/v1");
  size_t l = strlen(base);
  bool hasSlash = (l > 0 && base[l - 1] == '/');
  snprintf(out, outSize, "%s%s%s", base, hasSlash ? "" : "/", isEvent ? "events/batch" : "telemetry/batch");
}

bool buildTelemetryJson(const char *runName, const TelemetryRow *rows, uint8_t count, char *out, size_t outSize) {
  size_t len = 0;
  if (!appendFmt(out, outSize, len, "{\"device_id\":\"%s\",\"records\":[", cloudCfg.deviceId)) return false;
  char iso[24];
  getRtcIso(iso, sizeof(iso));
  for (uint8_t i = 0; i < count; i++) {
    const TelemetryRow &r = rows[i];
    if (i) if (!appendFmt(out, outSize, len, ",")) return false;
    if (!appendFmt(out, outSize, len,
      "{\"run_file\":\"%s\",\"line_index\":%lu,\"rtc_iso\":\"%s\",\"ms\":%lu,\"t1\":%s,\"u1\":%s,\"t2\":%s,\"u2\":%s,\"tavg\":%s,\"uavg\":%s,\"mask\":%u,\"step\":\"%s\",\"sd_state\":\"%s\",\"rtc_state\":\"%s\",\"run_state\":\"%s\"}",
      runName, (unsigned long)r.lineIndex, iso, (unsigned long)r.ms,
      r.t1, r.u1, r.t2, r.u2, r.tavg, r.uavg, (unsigned)r.mask, r.step,
      sdStateTxt(), rtcStateTxt(), runStateTxt())) return false;
  }
  if (!appendFmt(out, outSize, len, "]}")) return false;
  return true;
}

bool buildEventJson(const EventUploadRow *rows, uint8_t count, char *out, size_t outSize) {
  size_t len = 0;
  if (!appendFmt(out, outSize, len, "{\"device_id\":\"%s\",\"records\":[", cloudCfg.deviceId)) return false;
  for (uint8_t i = 0; i < count; i++) {
    const EventUploadRow &r = rows[i];
    if (i) if (!appendFmt(out, outSize, len, ",")) return false;
    if (!appendFmt(out, outSize, len,
      "{\"line_index\":%lu,\"rtc_iso\":\"%s\",\"event_type\":\"%s\",\"screen\":\"%s\",\"arg0\":%d,\"arg1\":%d,\"run_file\":\"%s\",\"current_step\":%u}",
      (unsigned long)r.lineIndex, r.rtcIso, r.eventType, r.screenName, (int)r.arg0, (int)r.arg1, r.runFile, (unsigned)r.step)) return false;
  }
  if (!appendFmt(out, outSize, len, "]}")) return false;
  return true;
}

int parseHttpCodeFromWindow() {
  const char *p = strstr(espRxWindow, "HTTP/1.1 ");
  if (!p) p = strstr(espRxWindow, "HTTP/1.0 ");
  if (!p) return -1;
  return atoi(p + 9);
}

void clearCloudJobFlags() {
  cloudBusy = false;
  cloudJobState = 0;
  cloudPayloadLen = 0;
  cloudHttpLen = 0;
  cloudPath[0] = '\0';
}

void cloudJobFinish(bool ok) {
  cloudLastJobDone = true;
  cloudLastJobOk = ok;
  netStats.lastHttpCode = cloudJobHttpCode;
  if (ok) {
    netStats.sent++;
    cloudBackoffMs = 1000;
    if (cloudHasCursorUpdate) {
      if (rtcOk) lastCloudSyncEpoch = rtc.now().unixtime();
      syncIndexSave(cloudNextCursor);
    }
  } else {
    netStats.failed++;
    netStats.retried++;
    cloudBackoffMs = (cloudBackoffMs < 60000UL) ? (cloudBackoffMs * 2UL) : 60000UL;
    if (cloudBackoffMs < 1000UL) cloudBackoffMs = 1000UL;
    if (cloudJobHttpCode < 0) {
      netState = NET_ERROR;
      lastNetAttemptMs = millis();
    }
  }
  Serial1.print("AT+CIPCLOSE\r\n");
  clearCloudJobFlags();
}

bool startCloudHttpJob(const char *path, const char *payload, bool isEvent, const UploadCursor &nextCursor, const char *fileName) {
  if (cloudBusy || netState != NET_ONLINE) return false;
  safeCopy(cloudPath, sizeof(cloudPath), path);
  safeCopy(cloudPayload, sizeof(cloudPayload), payload);
  cloudPayloadLen = (uint16_t)strlen(cloudPayload);
  cloudJobIsEvent = isEvent;
  cloudJobHttpCode = -1;
  cloudHasCursorUpdate = true;
  cloudNextCursor = nextCursor;
  safeCopy(activeRunUpload, sizeof(activeRunUpload), fileName ? fileName : "");
  char header[360];
  int h = snprintf(header, sizeof(header),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "User-Agent: MegaESP/1.0\r\n"
    "Connection: close\r\n"
    "Content-Type: application/json\r\n"
    "X-Device-Id: %s\r\n"
    "X-Api-Token: %s\r\n"
    "Content-Length: %u\r\n\r\n",
    cloudPath, cloudCfg.apiHost, cloudCfg.deviceId, cloudCfg.apiToken, (unsigned)cloudPayloadLen);
  if (h <= 0 || h >= (int)sizeof(header)) return false;
  cloudHttpLen = (uint16_t)((uint16_t)h + cloudPayloadLen);
  cloudBusy = true;
  cloudJobState = 1;
  cloudJobStartedMs = millis();
  cloudJobDeadlineMs = millis() + 7000UL;
  clearEspRxWindow();
  return true;
}

void cloudHttpJobTick() {
  if (!cloudBusy) return;
  while (Serial1.available()) appendEspRx((char)Serial1.read());
  unsigned long now = millis();
  if (cloudJobState == 1) {
    char cmd[120];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"SSL\",\"%s\",443", cloudCfg.apiHost);
    espSendCmd(cmd);
    cloudJobState = 2;
    cloudJobDeadlineMs = now + 7000UL;
  } else if (cloudJobState == 2) {
    if (espHas("OK") || espHas("CONNECT") || espHas("ALREADY CONNECTED")) {
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)cloudHttpLen);
      espSendCmd(cmd);
      cloudJobState = 3;
      cloudJobDeadlineMs = now + 4000UL;
    } else if (espHas("ERROR") || espHas("FAIL") || now > cloudJobDeadlineMs) {
      cloudJobHttpCode = -1;
      cloudJobFinish(false);
    }
  } else if (cloudJobState == 3) {
    if (espHas(">")) {
      clearEspRxWindow();
      char header[360];
      int h = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: MegaESP/1.0\r\n"
        "Connection: close\r\n"
        "Content-Type: application/json\r\n"
        "X-Device-Id: %s\r\n"
        "X-Api-Token: %s\r\n"
        "Content-Length: %u\r\n\r\n",
        cloudPath, cloudCfg.apiHost, cloudCfg.deviceId, cloudCfg.apiToken, (unsigned)cloudPayloadLen);
      if (h <= 0 || h >= (int)sizeof(header)) {
        cloudJobHttpCode = -1;
        cloudJobFinish(false);
        return;
      }
      Serial1.write((const uint8_t*)header, (size_t)h);
      Serial1.write((const uint8_t*)cloudPayload, cloudPayloadLen);
      cloudJobState = 4;
      cloudJobDeadlineMs = now + 5000UL;
    } else if (espHas("ERROR") || now > cloudJobDeadlineMs) {
      cloudJobHttpCode = -1;
      cloudJobFinish(false);
    }
  } else if (cloudJobState == 4) {
    if (espHas("SEND OK")) {
      cloudJobState = 5;
      cloudJobDeadlineMs = now + 9000UL;
    } else if (espHas("ERROR") || espHas("FAIL") || now > cloudJobDeadlineMs) {
      cloudJobHttpCode = -1;
      cloudJobFinish(false);
    }
  } else if (cloudJobState == 5) {
    int code = parseHttpCodeFromWindow();
    if (code > 0) cloudJobHttpCode = code;
    if (espHas("CLOSED") || now > cloudJobDeadlineMs) {
      bool ok = (cloudJobHttpCode >= 200 && cloudJobHttpCode < 300);
      cloudJobFinish(ok);
    }
  }
}

void forceNetReconnect() {
  netState = NET_CONNECTING;
  wifiStage = 0;
  wifiStageMs = 0;
  lastNetAttemptMs = millis();
  clearEspRxWindow();
}

void wifiAtManager() {
  while (Serial1.available()) appendEspRx((char)Serial1.read());
  if (cloudBusy) {
    cloudHttpJobTick();
  }
  if (!cloudCfg.enabled || !cloudConfigValid()) {
    netState = NET_OFF;
    wifiStage = 0;
    return;
  }

  unsigned long now = millis();
  if (netState == NET_ERROR) {
    if (now - lastNetAttemptMs < cloudBackoffMs) return;
    netState = NET_CONNECTING;
    wifiStage = 0;
  }
  if (netState == NET_OFF) {
    netState = NET_CONNECTING;
    wifiStage = 0;
  }

  if (netState != NET_CONNECTING) return;
  if (wifiStage == 0) {
    espSendCmd("AT");
    wifiStage = 1;
    wifiStageMs = now;
  } else if (wifiStage == 1) {
    if (espHas("OK")) {
      espSendCmd("ATE0");
      wifiStage = 2;
      wifiStageMs = now;
    } else if (now - wifiStageMs > 2000UL) {
      netState = NET_ERROR; lastNetAttemptMs = now;
    }
  } else if (wifiStage == 2) {
    if (espHas("OK")) {
      espSendCmd("AT+CWMODE=1");
      wifiStage = 3;
      wifiStageMs = now;
    } else if (espHas("ERROR") || now - wifiStageMs > 2000UL) {
      netState = NET_ERROR; lastNetAttemptMs = now;
    }
  } else if (wifiStage == 3) {
    if (espHas("OK")) {
      char cmd[136];
      if (cloudCfg.pass[0]) {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", cloudCfg.ssid, cloudCfg.pass);
      } else {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\"", cloudCfg.ssid);
      }
      espSendCmd(cmd);
      wifiStage = 4;
      wifiStageMs = now;
    } else if (espHas("ERROR") || now - wifiStageMs > 2000UL) {
      netState = NET_ERROR; lastNetAttemptMs = now;
    }
  } else if (wifiStage == 4) {
    if (espHas("WIFI GOT IP") || espHas("OK")) {
      espSendCmd("AT+CIPMUX=0");
      wifiStage = 5;
      wifiStageMs = now;
    } else if (espHas("FAIL") || espHas("ERROR") || now - wifiStageMs > 15000UL) {
      netState = NET_ERROR; lastNetAttemptMs = now;
    }
  } else if (wifiStage == 5) {
    if (espHas("OK")) {
      netState = NET_ONLINE;
      wifiStage = 0;
      clearEspRxWindow();
    } else if (espHas("ERROR") || now - wifiStageMs > 3000UL) {
      netState = NET_ERROR; lastNetAttemptMs = now;
    }
  }
}

void emitUiEvent(const char *eventType, int16_t arg0, int16_t arg1) {
  if (!ensureSdReady(false)) return;
  File f = SD.open("EVENTS.CSV", FILE_WRITE);
  if (!f) return;
  if (f.size() == 0) {
    f.println("ms;rtc_iso;event;screen;arg0;arg1;run_file;step");
  }
  char iso[24];
  getRtcIso(iso, sizeof(iso));
  f.print(millis());
  f.print(';');
  f.print(iso);
  f.print(';');
  f.print(eventType ? eventType : "evt");
  f.print(';');
  f.print(screenName(screen));
  f.print(';');
  f.print(arg0);
  f.print(';');
  f.print(arg1);
  f.print(';');
  f.print(currentFile);
  f.print(';');
  f.println(run.currentStep);
  f.close();
}

void printCfgStatus() {
  Serial.println(F("CFG STATUS"));
  Serial.print(F("WIFI_ENABLE=")); Serial.println(cloudCfg.enabled ? 1 : 0);
  Serial.print(F("WIFI_SSID=")); Serial.println(cloudCfg.ssid);
  Serial.print(F("WIFI_PASS=")); Serial.println(cloudCfg.pass[0] ? F("***") : F(""));
  Serial.print(F("API_HOST=")); Serial.println(cloudCfg.apiHost);
  Serial.print(F("API_PATH=")); Serial.println(cloudCfg.apiPath);
  Serial.print(F("API_TOKEN=")); Serial.println(cloudCfg.apiToken[0] ? "***" : "");
  Serial.print(F("DEVICE_ID=")); Serial.println(cloudCfg.deviceId);
  Serial.print(F("NET_STATE=")); Serial.println(netStateTxt());
}

void handleCfgCommand(char *line) {
  char *p = trimInPlace(line);
  if (strlen(p) < 3) return;
  if (!(toupper(p[0]) == 'C' && toupper(p[1]) == 'F' && toupper(p[2]) == 'G')) return;
  p += 3;
  p = trimInPlace(p);
  if (!*p) {
    Serial.println(F("CFG commands: WIFI_SSID/WIFI_PASS/API_HOST/API_PATH/API_TOKEN/DEVICE_ID/WIFI_ENABLE/SHOW/SAVE/TEST"));
    return;
  }
  char *space = strchr(p, ' ');
  char key[24];
  if (space) {
    size_t n = (size_t)(space - p);
    if (n >= sizeof(key)) n = sizeof(key) - 1;
    strncpy(key, p, n);
    key[n] = '\0';
    p = trimInPlace(space + 1);
  } else {
    safeCopy(key, sizeof(key), p);
    p = (char*)"";
  }

  if (cmpIgnoreCase(key, "SHOW") == 0) {
    printCfgStatus();
  } else if (cmpIgnoreCase(key, "SAVE") == 0) {
    saveConfigToEeprom();
    Serial.println(F("CFG saved"));
  } else if (cmpIgnoreCase(key, "TEST") == 0) {
    forceNetReconnect();
    Serial.println(F("CFG test reconnect"));
  } else if (cmpIgnoreCase(key, "WIFI_SSID") == 0) {
    safeCopy(cloudCfg.ssid, sizeof(cloudCfg.ssid), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "WIFI_PASS") == 0) {
    safeCopy(cloudCfg.pass, sizeof(cloudCfg.pass), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "API_HOST") == 0) {
    safeCopy(cloudCfg.apiHost, sizeof(cloudCfg.apiHost), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "API_PATH") == 0) {
    safeCopy(cloudCfg.apiPath, sizeof(cloudCfg.apiPath), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "API_TOKEN") == 0) {
    safeCopy(cloudCfg.apiToken, sizeof(cloudCfg.apiToken), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "DEVICE_ID") == 0) {
    safeCopy(cloudCfg.deviceId, sizeof(cloudCfg.deviceId), p);
    Serial.println(F("OK"));
  } else if (cmpIgnoreCase(key, "WIFI_ENABLE") == 0) {
    cloudCfg.enabled = (uint8_t)(atoi(p) ? 1 : 0);
    Serial.println(F("OK"));
  } else {
    Serial.println(F("Unknown CFG key"));
  }
}

void processSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialCmdLen > 0) {
        serialCmdLine[serialCmdLen] = '\0';
        handleCfgCommand(serialCmdLine);
        serialCmdLen = 0;
      }
    } else {
      if ((size_t)(serialCmdLen + 1) < sizeof(serialCmdLine)) {
        serialCmdLine[serialCmdLen++] = c;
      }
    }
  }
}

void cloudUploaderTick() {
  if (!cloudCfg.enabled || !cloudConfigValid()) return;
  if (netState != NET_ONLINE) return;
  if (cloudBusy) return;
  if (millis() - lastCloudTickMs < CLOUD_TICK_MS) return;
  lastCloudTickMs = millis();

  char runName[13];
  UploadCursor from;
  UploadCursor to;
  TelemetryRow rows[CLOUD_BATCH_MAX];
  uint8_t count = 0;
  if (findPendingRunForUpload(runName, from)) {
    if (readTelemetryBatch(runName, from, rows, CLOUD_BATCH_MAX, to, count) && count > 0) {
      if (buildTelemetryJson(runName, rows, count, cloudPayload, sizeof(cloudPayload))) {
        char endpoint[48];
        makeEndpointPath(false, endpoint, sizeof(endpoint));
        if (startCloudHttpJob(endpoint, cloudPayload, false, to, runName)) {
          netStats.pendingLines = count;
          return;
        }
      }
    }
  }

  UploadCursor evFrom;
  UploadCursor evTo;
  syncIndexLoad("EVENTS.CSV", evFrom);
  EventUploadRow eRows[CLOUD_BATCH_MAX];
  uint8_t eCount = 0;
  if (readEventBatch(evFrom, eRows, CLOUD_BATCH_MAX, evTo, eCount) && eCount > 0) {
    if (buildEventJson(eRows, eCount, cloudPayload, sizeof(cloudPayload))) {
      char endpoint[48];
      makeEndpointPath(true, endpoint, sizeof(endpoint));
      if (startCloudHttpJob(endpoint, cloudPayload, true, evTo, "EVENTS.CSV")) {
        netStats.pendingLines = eCount;
      }
    }
  }
}

void showWifiStatus() {
  lcd.clear();
  char l0[17], l1[17];
  snprintf(l0, sizeof(l0), "WF:%s HC:%d", netStateTxt(), netStats.lastHttpCode);
  snprintf(l1, sizeof(l1), "P:%lu S:%lu F:%lu", (unsigned long)netStats.pendingLines, (unsigned long)netStats.sent, (unsigned long)netStats.failed);
  print16(0, 0, l0);
  print16(0, 1, l1);
}

// ===== Relay control =====
void applyRelayMask(uint8_t mask) {
  relayMask = mask & 0x0F;
  for (byte i = 0; i < 4; i++) {
    bool on = relayMask & (1 << i);
    digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? !on : on);
  }
}

void updateThermostat(uint16_t tmin10, uint16_t tmax10, uint8_t baseMask) {
  uint8_t hb = thermoCfg.heaterRelayBit > 3 ? 2 : thermoCfg.heaterRelayBit;
  unsigned long now = millis();
  bool hysteresisMode = (tmax10 > tmin10);
  bool thresholdMode = (!hysteresisMode && tmin10 > 0);

  if (thresholdMode) {
    if (!haveValid) {
      heaterOn = false;
    } else {
      int t10 = (int)(tAvg * 10.0f);
      if (t10 >= (int)tmin10) {
        if (heaterOn) {
          heaterOn = false;
          heaterStateChangedMs = now;
        }
      } else {
        uint16_t intervalS = heaterOn ? thermoCfg.minOnSec : thermoCfg.minOffSec;
        if (intervalS == 0) intervalS = 1;
        unsigned long intervalMs = (unsigned long)intervalS * 1000UL;
        if (now - heaterStateChangedMs >= intervalMs) {
          heaterOn = !heaterOn;
          heaterStateChangedMs = now;
          if (heaterOn) heaterOnSinceMs = now;
        }
      }
    }
    if (heaterOn) baseMask |= (1 << hb);
    else baseMask &= ~(1 << hb);
  } else if (!hysteresisMode || !haveValid) {
    heaterOn = (baseMask & (1 << hb)) != 0;
    if (heaterOn) heaterOnSinceMs = now;
  } else {
    int t10 = (int)(tAvg * 10.0f);
    bool targetOn = heaterOn;
    int tmin = (int)tmin10;
    int tmax = (int)tmax10;
    if (!heaterOn && t10 < tmin) targetOn = true;
    if (heaterOn && t10 > tmax) targetOn = false;

    unsigned long minOnMs = (unsigned long)thermoCfg.minOnSec * 1000UL;
    unsigned long minOffMs = (unsigned long)thermoCfg.minOffSec * 1000UL;

    if (!heaterOn && targetOn) {
      if (now - heaterStateChangedMs >= minOffMs) {
        heaterOn = true;
        heaterStateChangedMs = now;
        heaterOnSinceMs = now;
      }
    } else if (heaterOn && !targetOn) {
      if (now - heaterStateChangedMs >= minOnMs) {
        heaterOn = false;
        heaterStateChangedMs = now;
      }
    }

    if (heaterOn && thermoCfg.safetyMaxSecOn > 0) {
      unsigned long safetyMs = (unsigned long)thermoCfg.safetyMaxSecOn * 1000UL;
      if (now - heaterOnSinceMs >= safetyMs && now - heaterStateChangedMs >= minOnMs) {
        heaterOn = false;
        heaterStateChangedMs = now;
      }
    }

    if (heaterOn) baseMask |= (1 << hb);
    else baseMask &= ~(1 << hb);
  }
  if (baseMask != relayMask) applyRelayMask(baseMask);
}

// ===== Sensor read =====
bool readDhtWithRetries(DHT &dht, float &tOut, float &hOut) {
  for (uint8_t i = 0; i < 3; i++) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(t) && !isnan(h)) {
      tOut = t;
      hOut = h;
      return true;
    }
    delay(50);
  }
  return false;
}

void readSensors() {
  unsigned long now = millis();
  unsigned long period = haveValid ? DHT_PERIOD_MS : DHT_FAIL_RETRY_MS;
  if (now - lastReadMs < period) return;
  lastReadMs = now;

  float th1 = NAN, hu1 = NAN, th2 = NAN, hu2 = NAN;
  bool ok1 = readDhtWithRetries(dht1, th1, hu1);
  bool ok2 = false;
  if (USE_DHT2) ok2 = readDhtWithRetries(dht2, th2, hu2);
  dht1Ok = ok1;
  dht2Ok = USE_DHT2 ? ok2 : true;

  if (!ok1) {
    if (!haveValid) return;
    return;
  }
  if (!USE_DHT2 || !ok2) {
    th2 = th1;
    hu2 = hu1;
  }
  t1 = th1; h1 = hu1; t2 = th2; h2 = hu2;
  tAvg = (t1 + t2) * 0.5f;
  hAvg = (h1 + h2) * 0.5f;
  haveValid = true;
  lastValidSensorMs = now;
}

void logSample(const StepData &st) {
  if (!haveValid) return;
  unsigned long now = millis();
  if (now - lastLogMs < LOG_PERIOD_MS) return;
  lastLogMs = now;
  LogRecord rec;
  rec.ms = millis();
  rec.t1_10 = (int16_t)(t1 * 10.0f);
  rec.h1_10 = (int16_t)(h1 * 10.0f);
  rec.t2_10 = (int16_t)(t2 * 10.0f);
  rec.h2_10 = (int16_t)(h2 * 10.0f);
  rec.tAvg_10 = (int16_t)(tAvg * 10.0f);
  rec.hAvg_10 = (int16_t)(hAvg * 10.0f);
  rec.mask = relayMask;
  safeCopy(rec.step, sizeof(rec.step), st.label);
  queueLogRecord(rec);
}

// ===== UI =====
void showMenu() {
  char line0[17];
  char line1[17];
  snprintf(line0, sizeof(line0), "%-16s", MENU_ITEMS[menuIndex]);
  print16(0, 0, line0);
  const char *sdTxt = "FAIL";
  if (sdState == SD_READY) sdTxt = "OK  ";
  else if (sdState == SD_DEGRADED) sdTxt = "DEG ";
  const char *rtcTxt = "FAIL";
  if (rtcOk) rtcTxt = rtcLostPowerOrInvalid ? "SET " : "OK  ";
  snprintf(line1, sizeof(line1), "SD:%s RTC:%s", sdTxt, rtcTxt);
  print16(0, 1, line1);
}

void showExpList() {
  lcd.clear();
  if (sdFileCount == 0) {
    print16(0, 0, "Sem exp no SD  ");
    print16(0, 1, "                ");
    return;
  }
  char line0[17];
  snprintf(line0, sizeof(line0), "%2u/%-2u %-8s", expFileIndex + 1, sdFileCount, expFiles[expFileIndex]);
  print16(0, 0, line0);
  print16(0, 1, "SD             ");
}

void showIntList() {
  lcd.clear();
  if (INTERNAL_COUNT == 0) {
    print16(0, 0, "Sem interno    ");
    print16(0, 1, "                ");
    return;
  }
  char line0[17];
  snprintf(line0, sizeof(line0), "%2u/%-2u %-8s", intFileIndex + 1, INTERNAL_COUNT, INTERNAL_EXPS[intFileIndex].name);
  print16(0, 0, line0);
  print16(0, 1, "Interno        ");
}

void showServiceMenu() {
  lcd.clear();
  char line0[17];
  snprintf(line0, sizeof(line0), "%2u/%-2u %-10s", serviceIndex + 1, NSERVICE, SERVICE_ITEMS[serviceIndex]);
  print16(0, 0, line0);
  print16(0, 1, "OK=Entrar Back ");
}

void showConfigMenu() {
  lcd.clear();
  char line0[17];
  snprintf(line0, sizeof(line0), "%2u/%-2u %-10s", configIndex + 1, NCONFIG, CONFIG_ITEMS[configIndex]);
  print16(0, 0, line0);
  char line1[17];
  snprintf(line1, sizeof(line1), "INT:%us       ", thermoCfg.minOnSec);
  print16(0, 1, line1);
}

void showHeaterIntervalConfig() {
  lcd.clear();
  char line0[17], line1[17];
  snprintf(line0, sizeof(line0), "Aqec INT %4us", heaterIntervalEdit);
  snprintf(line1, sizeof(line1), "OK=Salvar Back ");
  print16(0, 0, line0);
  print16(0, 1, line1);
}

void showTimeSet() {
  lcd.clear();
  if (!rtcOk) {
    print16(0, 0, "RTC FAIL       ");
    print16(0, 1, "Back menu      ");
    return;
  }

  static const char *FIELD_NAME[] = {"Y", "M", "D", "h", "m", "s"};
  char line0[17], line1[17];
  snprintf(line0, sizeof(line0), "%04u-%02u-%02u", timeSet.year, timeSet.month, timeSet.day);
  snprintf(line1, sizeof(line1), "%02u:%02u:%02u F:%s", timeSet.hour, timeSet.minute, timeSet.second, FIELD_NAME[timeSet.field]);
  print16(0, 0, line0);
  print16(0, 1, line1);
}

void showSensorTest(bool cfgPage) {
  lcd.clear();
  char l0[17], l1[17], a[8], b[8], c[8], d[8];
  if (!cfgPage) {
    if (dht1Ok && haveValid) {
      fmtFloat1(a, t1); fmtFloat1(b, h1);
      snprintf(l0, sizeof(l0), "1OK T%s U%s", a, b);
    } else {
      snprintf(l0, sizeof(l0), "1ERR sem leitura");
    }
    if (!USE_DHT2) {
      unsigned long age = haveValid ? ((millis() - lastValidSensorMs) / 1000UL) : 0;
      snprintf(l1, sizeof(l1), "2OFF Age:%lus", age);
    } else if (dht2Ok && haveValid) {
      fmtFloat1(c, t2); fmtFloat1(d, h2);
      snprintf(l1, sizeof(l1), "2OK T%s U%s", c, d);
    } else {
      snprintf(l1, sizeof(l1), "2ERR sem leitura");
    }
  } else {
    snprintf(l0, sizeof(l0), "On%us Off%us", thermoCfg.minOnSec, thermoCfg.minOffSec);
    snprintf(l1, sizeof(l1), "H%u M%us D%u", thermoCfg.heaterRelayBit, thermoCfg.safetyMaxSecOn, droppedLogsCount);
  }
  print16(0, 0, l0);
  print16(0, 1, l1);
}

uint8_t relayTestSelected = 0;
uint8_t relayTestMask = 0;

void showRelayTest() {
  lcd.clear();
  char maskTxt[5];
  maskToChars(relayTestMask, maskTxt);
  char l0[17], l1[17];
  bool on = (relayTestMask & (1 << relayTestSelected)) != 0;
  snprintf(l0, sizeof(l0), "Sel R%u:%s", relayTestSelected + 1, on ? "ON " : "OFF");
  snprintf(l1, sizeof(l1), "Mask %s", maskTxt);
  print16(0, 0, l0);
  print16(0, 1, l1);
}

void drawRunning(const StepData &st) {
  static unsigned long lastPageMs = 0;
  static bool showTempPage = true;
  if (millis() - lastPageMs > 3000UL) {
    lastPageMs = millis();
    showTempPage = !showTempPage;
  }
  char line0[17], line1[17], a[8], b[8], c[8], d[8];
  uint32_t expSec = (millis() - run.expStartMs - run.totalPauseMs) / 1000UL;
  uint8_t hh = (expSec / 3600UL) % 24;
  uint8_t mm = (expSec / 60UL) % 60;
  uint8_t ss = expSec % 60;
  char sdMark = (sdState == SD_READY) ? ' ' : '!';
  snprintf(line0, sizeof(line0), "S%s%c%02u:%02u:%02u", st.label, sdMark, hh, mm, ss);
  if (!haveValid) {
    snprintf(line1, sizeof(line1), "Sem leitura   ");
  } else if (showTempPage) {
    fmtFloat1(a, t1); fmtFloat1(b, t2);
    snprintf(line1, sizeof(line1), "1T%s 2T%s", a, b);
  } else {
    fmtFloat1(c, h1); fmtFloat1(d, h2);
    snprintf(line1, sizeof(line1), "1U%s 2U%s", c, d);
  }
  if (noticeActive()) {
    print16(0, 0, noticeLine0);
    print16(0, 1, noticeLine1);
  } else {
    print16(0, 0, line0);
    print16(0, 1, line1);
  }
}

// ===== Run control =====
bool startExperiment() {
  if (!openRunFile()) return false;
  run.active = true;
  run.paused = false;
  run.waitRetrieval = false;
  run.stepCount = meta.stepCount;
  run.currentStep = 0;
  run.retrievalIndex = 0;
  run.expStartMs = millis();
  run.totalPauseMs = 0;
  run.pausedAt = 0;
  stepActive = false;
  stepDone = false;
  lastLogMs = 0;
  lastFlushTryMs = 0;
  logFlushCounter = 0;
  resetLogQueue();
  sdDisconnectNotice = false;
  sdReconnectNotice = false;
  noticeUntilMs = 0;
  heaterOn = false;
  unsigned long offMs = (unsigned long)thermoCfg.minOffSec * 1000UL;
  heaterStateChangedMs = millis() - offMs;
  heaterOnSinceMs = heaterStateChangedMs;
  if (currentSource == SRC_SD) {
    if (!openLogFile()) setSdState(SD_DEGRADED);
  } else {
    logOpen = false;
  }
  emitUiEvent("run_start", run.stepCount, 0);
  return true;
}

void stopExperiment(const char *msg) {
  run.active = false;
  stepActive = false;
  stepDone = false;
  if (runFile) runFile.close();
  if (logOpen) { logFile.close(); logOpen = false; }
  emitUiEvent("run_stop", run.currentStep, 0);
  lcd.clear();
  print16(0, 0, "Parado");
  if (msg) print16(0, 1, msg);
}

void finishExperiment() {
  uint32_t expSec = (millis() - run.expStartMs - run.totalPauseMs) / 1000UL;
  uint8_t hh = (expSec / 3600UL) % 24;
  uint8_t mm = (expSec / 60UL) % 60;
  uint8_t ss = expSec % 60;
  char timebuf[12];
  snprintf(timebuf, sizeof(timebuf), "%02u:%02u:%02u", hh, mm, ss);

  run.active = false;
  stepActive = false;
  stepDone = false;
  if (runFile) runFile.close();
  if (logOpen) { logFile.close(); logOpen = false; }
  emitUiEvent("run_done", run.currentStep, 0);

  lcd.clear();
  print16(0, 0, "Exp finished");
  for (uint8_t i = 0; i < 3; i++) {
    print16(0, 1, timebuf);
    delay(500);
    print16(0, 1, "                ");
    delay(500);
  }
  screen = SCREEN_MENU;
  showMenu();
}

// ===== Buttons =====
void handleButtons(StepData &st) {
  bool eU = edge(bU), eD = edge(bD), eO = edge(bO), eB = edge(bB);
  if (screen == SCREEN_MENU) {
    if (eU && pressed(bU)) { menuIndex = (menuIndex + NITEMS - 1) % NITEMS; showMenu(); }
    if (eD && pressed(bD)) { menuIndex = (menuIndex + 1) % NITEMS; showMenu(); }
    if (eO && pressed(bO)) {
      if (menuIndex == 0) { scanExperimentFiles(); expFileIndex = 0; screen = SCREEN_EXP_LIST; showExpList(); }
      else if (menuIndex == 1) { intFileIndex = 0; screen = SCREEN_INT_LIST; showIntList(); }
      else if (menuIndex == 2 && !run.active) { serviceIndex = 0; screen = SCREEN_SERVICE_MENU; showServiceMenu(); }
      else if (menuIndex == 3 && !run.active) { loadTimeSetFromRtc(); screen = SCREEN_TIME_SET; showTimeSet(); }
    }
  } else if (screen == SCREEN_EXP_LIST) {
    if (sdFileCount == 0) { if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); } return; }
    if (eU && pressed(bU)) { expFileIndex = (expFileIndex + sdFileCount - 1) % sdFileCount; showExpList(); }
    if (eD && pressed(bD)) { expFileIndex = (expFileIndex + 1) % sdFileCount; showExpList(); }
    if (eO && pressed(bO)) {
      bool ok = loadExperiment(expFiles[expFileIndex]);
      if (ok) {
        screen = SCREEN_RUNNING;
        if (!startExperiment()) {
          lcd.clear(); print16(0, 0, "Falha abrir"); delay(700);
          screen = SCREEN_MENU; showMenu();
        }
      } else {
        lcd.clear(); print16(0, 0, "Falha exp"); delay(700); showExpList();
      }
    }
    if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); }
  } else if (screen == SCREEN_INT_LIST) {
    if (INTERNAL_COUNT == 0) { if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); } return; }
    if (eU && pressed(bU)) { intFileIndex = (intFileIndex + INTERNAL_COUNT - 1) % INTERNAL_COUNT; showIntList(); }
    if (eD && pressed(bD)) { intFileIndex = (intFileIndex + 1) % INTERNAL_COUNT; showIntList(); }
    if (eO && pressed(bO)) {
      if (loadExperimentInternal(intFileIndex)) {
        screen = SCREEN_RUNNING;
        if (!startExperiment()) {
          lcd.clear(); print16(0, 0, "Falha abrir"); delay(700);
          screen = SCREEN_MENU; showMenu();
        }
      } else {
        lcd.clear(); print16(0, 0, "Falha exp"); delay(700); showIntList();
      }
    }
    if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); }
  } else if (screen == SCREEN_SERVICE_MENU) {
    if (eU && pressed(bU)) { serviceIndex = (serviceIndex + NSERVICE - 1) % NSERVICE; showServiceMenu(); }
    if (eD && pressed(bD)) { serviceIndex = (serviceIndex + 1) % NSERVICE; showServiceMenu(); }
    if (eO && pressed(bO)) {
      if (serviceIndex == 0) {
        sensorCfgPage = false;
        screen = SCREEN_SENSOR_TEST;
        showSensorTest(sensorCfgPage);
      } else if (serviceIndex == 1) {
        relayTestSelected = 0;
        relayTestMask = 0;
        applyRelayMask(relayTestMask);
        screen = SCREEN_RELAY_TEST;
        showRelayTest();
      } else if (serviceIndex == 2) {
        configIndex = 0;
        screen = SCREEN_CONFIG_MENU;
        showConfigMenu();
      } else if (serviceIndex == 3) {
        screen = SCREEN_WIFI_STATUS;
        showWifiStatus();
      } else if (serviceIndex == 4) {
        loadThermoConfigChain();
        lcd.clear();
        print16(0, 0, "CFG recarregado");
        print16(0, 1, "                ");
        delay(700);
        showServiceMenu();
      }
    }
    if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); }
  } else if (screen == SCREEN_SENSOR_TEST) {
    if (eO && pressed(bO)) {
      sensorCfgPage = !sensorCfgPage;
      showSensorTest(sensorCfgPage);
    }
    if (eB && pressed(bB)) { screen = SCREEN_SERVICE_MENU; showServiceMenu(); }
  } else if (screen == SCREEN_RELAY_TEST) {
    if (eU && pressed(bU)) { relayTestSelected = (relayTestSelected + 3) % 4; showRelayTest(); }
    if (eD && pressed(bD)) { relayTestSelected = (relayTestSelected + 1) % 4; showRelayTest(); }
    if (eO && pressed(bO)) {
      relayTestMask ^= (1 << relayTestSelected);
      applyRelayMask(relayTestMask);
      showRelayTest();
    }
    if (eB && pressed(bB)) {
      relayTestMask = 0;
      applyRelayMask(0);
      screen = SCREEN_SERVICE_MENU;
      showServiceMenu();
    }
  } else if (screen == SCREEN_WIFI_STATUS) {
    if (eO && pressed(bO)) {
      forceNetReconnect();
      emitUiEvent("wifi_test", 0, 0);
      showWifiStatus();
    }
    if (eU && pressed(bU)) showWifiStatus();
    if (eD && pressed(bD)) showWifiStatus();
    if (eB && pressed(bB)) { screen = SCREEN_SERVICE_MENU; showServiceMenu(); }
  } else if (screen == SCREEN_CONFIG_MENU) {
    if (eU && pressed(bU)) { configIndex = (configIndex + NCONFIG - 1) % NCONFIG; showConfigMenu(); }
    if (eD && pressed(bD)) { configIndex = (configIndex + 1) % NCONFIG; showConfigMenu(); }
    if (eO && pressed(bO)) {
      if (configIndex == 0) {
        heaterIntervalEdit = thermoCfg.minOnSec;
        if (heaterIntervalEdit < 1) heaterIntervalEdit = 1;
        if (heaterIntervalEdit > 600) heaterIntervalEdit = 600;
        screen = SCREEN_CONFIG_HEATER_INTERVAL;
        showHeaterIntervalConfig();
      }
    }
    if (eB && pressed(bB)) { screen = SCREEN_SERVICE_MENU; showServiceMenu(); }
  } else if (screen == SCREEN_CONFIG_HEATER_INTERVAL) {
    if (eU && pressed(bU)) { if (heaterIntervalEdit < 600) heaterIntervalEdit++; showHeaterIntervalConfig(); }
    if (eD && pressed(bD)) { if (heaterIntervalEdit > 1) heaterIntervalEdit--; showHeaterIntervalConfig(); }
    if (eO && pressed(bO)) {
      thermoCfg.minOnSec = heaterIntervalEdit;
      thermoCfg.minOffSec = heaterIntervalEdit;
      saveThermoToEeprom();
      lcd.clear();
      print16(0, 0, "Intervalo salvo");
      print16(0, 1, "                ");
      delay(700);
      screen = SCREEN_CONFIG_MENU;
      showConfigMenu();
    }
    if (eB && pressed(bB)) { screen = SCREEN_CONFIG_MENU; showConfigMenu(); }
  } else if (screen == SCREEN_TIME_SET) {
    if (!rtcOk) {
      if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); }
      return;
    }
    if (eU && pressed(bU)) { adjustTimeField(+1); showTimeSet(); }
    if (eD && pressed(bD)) { adjustTimeField(-1); showTimeSet(); }
    if (eO && pressed(bO)) {
      if (timeSet.field < 5) {
        timeSet.field++;
        showTimeSet();
      } else {
        if (saveTimeSetToRtc()) {
          emitUiEvent("time_set", timeSet.hour, timeSet.minute);
          lcd.clear();
          print16(0, 0, "Hora salva");
          print16(0, 1, "                ");
          delay(700);
        } else {
          lcd.clear();
          print16(0, 0, "Falha RTC");
          print16(0, 1, "                ");
          delay(700);
        }
        screen = SCREEN_MENU;
        showMenu();
      }
    }
    if (eB && pressed(bB)) { screen = SCREEN_MENU; showMenu(); }
  } else if (screen == SCREEN_RUNNING) {
    if (eB && pressed(bB)) {
      screen = SCREEN_CONFIRM_STOP;
      lcd.clear();
      print16(0, 0, "Parar experim?");
      print16(0, 1, "OK=Sim Back=Nao");
    }
    if (eO && pressed(bO)) {
      run.paused = !run.paused;
      if (run.paused) run.pausedAt = millis();
      else run.totalPauseMs += millis() - run.pausedAt;
    }
  } else if (screen == SCREEN_CONFIRM_STOP) {
    if (eO && pressed(bO)) { stopExperiment("Stop"); screen = SCREEN_MENU; showMenu(); }
    if (eB && pressed(bB)) { screen = SCREEN_RUNNING; }
  } else if (screen == SCREEN_RETRIEVAL) {
    if (eO && pressed(bO)) {
      run.waitRetrieval = false;
      run.paused = false;
      run.retrievalIndex++;
      run.totalPauseMs += millis() - run.pausedAt;
      screen = SCREEN_RUNNING;
    }
    if (eB && pressed(bB)) { stopExperiment("Stop"); screen = SCREEN_MENU; showMenu(); }
  }
}

// ===== Setup/Loop =====
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  for (byte i = 0; i < 4; i++) pinMode(RELAY_PINS[i], OUTPUT);
  applyRelayMask(0);

  lcd.begin(16, 2);
  lcd.clear();
  print16(0, 0, "Init SD...     ");
  print16(0, 1, "                ");
  Wire.begin();
  rtcOk = rtc.begin();
  rtcLostPowerOrInvalid = rtcOk ? !rtc.isrunning() : true;
  if (initSD()) setSdState(SD_READY);
  else setSdState(SD_UNAVAILABLE);
  lastSdAttemptMs = millis();
  loadThermoConfigChain();
  lcd.clear();
  showMenu();

  if (DHT_USE_PULLUP) {
    pinMode(DHT1_PIN, INPUT_PULLUP);
    if (USE_DHT2) pinMode(DHT2_PIN, INPUT_PULLUP);
  }
  dht1.begin();
  dht2.begin();
  delay(1000); // allow DHT22s to stabilize before first read
  readSensors();

  Serial1.begin(115200); // ESP8266 AT
  clearEspRxWindow();
  if (cloudCfg.enabled && cloudConfigValid()) forceNetReconnect();
  Serial.println(F("CFG commands ready (type: CFG SHOW)"));
}

void loop() {
  static StepData currentStep;
  static unsigned long lastSensorScreenMs = 0;
  static unsigned long lastWifiScreenMs = 0;

  if (!run.active && (screen == SCREEN_MENU || screen == SCREEN_EXP_LIST || screen == SCREEN_SERVICE_MENU)) {
    bool prev = sdOk;
    checkSD();
    if (sdOk != prev) {
      scanExperimentFiles();
      if (screen == SCREEN_MENU) showMenu();
      else if (screen == SCREEN_EXP_LIST) showExpList();
      else if (screen == SCREEN_SERVICE_MENU) showServiceMenu();
    }
  }

  if (sdDisconnectNotice) {
    showNotice("SD desconectado", "rodando sem log", 1500);
    sdDisconnectNotice = false;
  }
  if (sdReconnectNotice) {
    showNotice("SD reconectado", "sincronizando", 1500);
    sdReconnectNotice = false;
  }

  processSerialCommands();
  wifiAtManager();
  cloudUploaderTick();

  handleButtons(currentStep);
  readSensors();

  if (screen == SCREEN_SENSOR_TEST && millis() - lastSensorScreenMs > 1000UL) {
    lastSensorScreenMs = millis();
    showSensorTest(sensorCfgPage);
  }
  if (screen == SCREEN_WIFI_STATUS && millis() - lastWifiScreenMs > 1000UL) {
    lastWifiScreenMs = millis();
    showWifiStatus();
  }

  if ((run.active && currentSource == SRC_SD) || logCount > 0) {
    if (ensureSdReady(false)) {
      if (sdState != SD_READY) setSdState(SD_READY);
    }
    processLogFlush();
  }

  if (screen == SCREEN_RUNNING && run.active && !run.paused) {
    if (!stepActive) {
      if (readNextStep(currentStep)) {
        stepActive = true;
        stepDone = false;
        stepStartMs = millis();
        uint16_t unitMs = meta.stepUnitMs ? meta.stepUnitMs : (uint16_t)STEP_UNIT_MS_DEFAULT;
        stepDurationMs = (unsigned long)currentStep.seconds * unitMs;
        run.currentStep++;
        applyRelayMask(currentStep.mask);
      } else {
        if (run.currentStep < run.stepCount) {
          stopExperiment("SD falha");
          screen = SCREEN_MENU;
          showMenu();
        } else {
          finishExperiment();
          return;
        }
      }
    } else {
      if (millis() - stepStartMs >= stepDurationMs) {
        stepActive = false;
      }
    }

    // retrieval pause
    if (meta.intervalMin > 0 && meta.retrievals > 0 && !run.waitRetrieval) {
      unsigned long expMs = millis() - run.expStartMs - run.totalPauseMs;
      unsigned long nextStop = (unsigned long)(run.retrievalIndex + 1) * meta.intervalMin * 60000UL;
      if (expMs >= nextStop && run.retrievalIndex < meta.retrievals) {
        run.waitRetrieval = true;
        run.paused = true;
        run.pausedAt = millis();
        lcd.clear();
        print16(0, 0, "Retirada");
        print16(0, 1, "OK=Sim Back=Nao");
        screen = SCREEN_RETRIEVAL;
      }
    }

    updateThermostat(currentStep.tmin10, currentStep.tmax10, currentStep.mask);
    if (haveValid) logSample(currentStep);
    drawRunning(currentStep);
  }
}
