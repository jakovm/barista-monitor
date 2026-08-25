#include <M5Unified.h>
#include <Preferences.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include "rtc_stamp.h"

static constexpr const char* NAMESPACE = "coffee";
static constexpr const char* KEY_LAST_DATE = "lastYmd";
static constexpr const char* KEY_CLEANER = "cleaner";
static constexpr const char* KEY_VOLT_EMA = "voltEma";
static constexpr const char* KEY_DIAG_VERSION = "diagVer";
static constexpr const char* KEY_RTC_STAMP = "rtcStamp";
/** Letzter gerenderter Tages-/Akku-Stand (überlebt Deep-Sleep). */
static constexpr const char* KEY_RENDER_DAYS = "rDays";
static constexpr const char* KEY_RENDER_BAT = "rBat";
/** Ringpuffer mit Boot-Diagnosen — nach einem Freeze am Gerät auslesbar. */
static constexpr const char* KEY_DIAG_LOG = "diagLog";
static constexpr const char* KEY_DIAG_HEAD = "diagHead";
static constexpr uint8_t DIAG_VERSION = 2;
static constexpr uint8_t DIAG_SLOTS = 8;

static constexpr const char* CLEANERS[] = {"Jakov", "Nina", "Guest"};
static constexpr uint8_t CLEANER_COUNT = 3;

static constexpr uint8_t CLEAN_DAYS_GRAY_START = 5;
static constexpr uint8_t CLEAN_DAYS_INVERT = 7;
static constexpr int SCREEN_W = 200;
static constexpr int SCREEN_H = 200;
static constexpr int SCREEN_MARGIN = 4;
static constexpr int DAY_BAND_H = 36;
static constexpr int DAY_BAR_H = 14;
static constexpr int BEAN_W = 22;
static constexpr int BEAN_H = 12;
/** Nach Nutzer-Interaktion wach bleiben (Menü, Reinigung markieren). */
static constexpr uint16_t AWAKE_MS = 60000;
/**
 * Nach stillem RTC-Wake (gleicher Kalendertag, kein Display-Update):
 * kurz wach, damit ein Tastendruck noch greift, dann wieder aus.
 */
static constexpr uint16_t HEARTBEAT_AWAKE_MS = 8000;
/**
 * Harte Obergrenze für ein Wachfenster. Ohne die hält ein klemmender oder
 * floatender Tasten-GPIO das Gerät über userActive() dauerhaft wach, bis der
 * Akku leer ist — von außen nicht von einem Freeze zu unterscheiden.
 */
static constexpr uint32_t MAX_AWAKE_MS = 180000;
/** Task-WDT: bei hängendem E-Paper-Refresh Neustart statt Dauer-Freeze. */
static constexpr uint32_t WDT_TIMEOUT_MS = 30000;
/**
 * CoreInk-Idle = Power-Hold trennen + BM8563-Timer, aber selbst gebaut.
 * M5.Power.timerSleep() ist dafür unbrauchbar:
 * - _powerOff() pulst POWER_HOLD 5x LOW/HIGH und endet HIGH; auf dem
 *   latch-basierten CoreInk muss der Pin einmal LOW und LOW bleiben.
 * - esp_sleep_enable_ext0_wakeup(GPIO19) schlägt fehl (kein RTC-GPIO),
 *   deshalb landet die Bibliothek in esp_light_sleep_start() statt Deep-Sleep.
 * - Der I2C-Write des Timers wird nirgends verifiziert.
 *
 * BM8563 Countdown: max. 255 Minuten → Cap 4 h 15 min.
 */
static constexpr uint32_t MAX_SLEEP_SEC = 255u * 60u;
static constexpr uint32_t MIN_SLEEP_SEC = 5;
/** Ersatztakt, wenn die RTC nicht verlässlich wecken kann. */
static constexpr uint32_t RTC_FALLBACK_SLEEP_SEC = 3600;
/** Häppchengröße der Rückfallebene. Kurz genug, um die RTC oft neu zu prüfen. */
static constexpr uint32_t SLEEP_CHUNK_SEC = 60;
/** BM8563 (PCF8563-kompatibel) am internen I2C-Bus. */
static constexpr uint8_t RTC_ADDR = 0x51;
static constexpr uint32_t RTC_FREQ = 400000;
static constexpr uint8_t RTC_REG_STATUS2 = 0x01;
static constexpr uint8_t RTC_REG_SECONDS = 0x02;
static constexpr uint8_t RTC_REG_TIMER_CTL = 0x0E;
static constexpr uint8_t RTC_REG_TIMER_CNT = 0x0F;
/** CoreInk POWER_HOLD: LOW trennt die Versorgung. */
static constexpr uint8_t POWER_HOLD_PIN = 12;
/** E-Paper BUSY (GDEW0154 auf CoreInk): LOW = Panel arbeitet noch. */
static constexpr uint8_t EPD_BUSY_PIN = 4;
/** waitDisplay() gibt nach 1 s auf; ein Full-Refresh dauert länger. */
static constexpr uint32_t EPD_BUSY_TIMEOUT_MS = 8000;
/** PWR-Taste. Einziger RTC-fähiger Wake-Pin am CoreInk (GPIO5/19 sind es nicht). */
static constexpr gpio_num_t PWR_BTN_PIN = GPIO_NUM_27;
/** Diagnose-Screen: Wahlhebel-Mitte so lange halten. */
static constexpr uint32_t DIAG_HOLD_MS = 2000;
static constexpr bool DEMO_DAY_PREVIEW = false;
static constexpr uint8_t DEMO_DAY_COUNT = 10;
static constexpr uint16_t DEMO_DAY_MS = 3000;
static constexpr bool DEMO_BATTERY_PREVIEW = false;
static constexpr uint8_t DEMO_BATTERY_START_DAYS = 14;
static constexpr uint16_t DEMO_BATTERY_MS = 3000;
static constexpr float BAT_WARN_DAYS = 10.0f;
static constexpr uint8_t BAT_ALARM_DAYS = 7;
static constexpr float BAT_FULL_V = 4.10f;
static constexpr float BAT_EMPTY_V = 3.35f;
/** Kalenderwechsel (Lokalzeit Mitternacht). */
static constexpr int DAILY_ROLLOVER_HOUR = 0;
static constexpr float BAT_EMA_PERSIST_DELTA = 0.02f;

Preferences prefs;
bool selecting = false;
uint8_t picker = 0;
bool screenDirty = true;
int daysSinceClean = 0;
uint32_t lastCleanYmd = 0;
uint8_t lastCleaner = 0;
bool inverted = false;
bool batteryLow = false;
float batteryEstDays = 90.0f;
bool batteryDemoActive = false;
int lastRenderedDaysSinceClean = -1;
bool lastRenderedBatteryLow = false;
/** Effektive Wachzeit bis Power-Off (Normal vs. stiller Heartbeat). */
uint32_t awakeLimitMs = AWAKE_MS;
bool showingDiag = false;
bool rtcUsable = false;

/**
 * Ein Boot-Eintrag. Bewusst klein und gepackt, damit acht Stück als ein
 * NVS-Blob durchgehen und der Freeze-Grund nach dem Freeze noch da ist.
 */
struct __attribute__((packed)) BootRecord {
  uint32_t ymd;        // 0 = RTC beim Boot nicht lesbar
  uint16_t hm;         // hhmm
  uint16_t voltMv;
  uint16_t sleepSec10; // geplante Sleep-Dauer / 10
  uint8_t resetReason;
  uint8_t wakeCause;
  uint8_t flags;
  uint8_t seq;
};

static constexpr uint8_t DIAG_F_RTC_READ = 0x01;   // I2C-Read hat funktioniert
static constexpr uint8_t DIAG_F_RTC_PLAUS = 0x02;  // Datum/Uhrzeit plausibel
static constexpr uint8_t DIAG_F_RTC_VLOW = 0x04;   // VL-Bit: Uhr war stromlos
static constexpr uint8_t DIAG_F_RTC_RESET = 0x08;  // aus Build-Stamp gesetzt
static constexpr uint8_t DIAG_F_TIMER_OK = 0x10;   // Weckzeit zurückgelesen, steht
static constexpr uint8_t DIAG_F_PWROFF_FAIL = 0x20;// Power-Hold hat nicht getrennt
static constexpr uint8_t DIAG_F_CHUNKED = 0x40;    // Rückfallebene Light-Sleep-Häppchen

BootRecord diagLog[DIAG_SLOTS];
uint8_t diagHead = 0;

struct Ymd {
  int year;
  int month;
  int day;
};

/**
 * 0 = ungültig. Ohne diese Prüfung wird aus einem fehlgeschlagenen RTC-Read
 * (day = -1) über den uint32-Cast der Schlüssel 20000099, und der landet
 * dauerhaft in NVS — Tageszähler danach im Tausenderbereich.
 */
uint32_t ymdToKey(const Ymd& ymd) {
  if (ymd.year < 1970 || ymd.year > 2099
   || ymd.month < 1 || ymd.month > 12
   || ymd.day < 1 || ymd.day > 31) {
    return 0;
  }
  return (uint32_t)ymd.year * 10000u + (uint32_t)ymd.month * 100u + (uint32_t)ymd.day;
}

Ymd keyToYmd(uint32_t key) {
  return {
    (int)(key / 10000u),
    (int)((key / 100u) % 100u),
    (int)(key % 100u),
  };
}

/**
 * Bei I2C-Fehler liefert M5.Rtc.getDateTime() die default-konstruierte
 * Struktur (year 2000, date/hours -1) zurück, ohne das anzuzeigen. Deshalb
 * überall den bool-Rückgabewert auswerten statt der Struktur zu vertrauen.
 */
bool readRtc(m5::rtc_datetime_t* out) {
  return M5.Rtc.getDateTime(out);
}

bool rtcPlausible(const m5::rtc_datetime_t& dt) {
  return dt.date.year >= 2024 && dt.date.year <= 2036
      && dt.date.month >= 1 && dt.date.month <= 12
      && dt.date.date >= 1 && dt.date.date <= 31
      && dt.time.hours >= 0 && dt.time.hours <= 23
      && dt.time.minutes >= 0 && dt.time.minutes <= 59
      && dt.time.seconds >= 0 && dt.time.seconds <= 59;
}

/** Kalendertag laut RTC. false = nicht lesbar/unplausibel → nichts speichern. */
bool calendarToday(Ymd* out) {
  m5::rtc_datetime_t dt;
  if (!readRtc(&dt) || !rtcPlausible(dt)) {
    return false;
  }
  *out = {dt.date.year, dt.date.month, dt.date.date};
  return true;
}

void formatRtcDateTime(char* out, size_t outLen) {
  m5::rtc_datetime_t dt;
  if (!readRtc(&dt)) {
    snprintf(out, outLen, "RTC ?");
    return;
  }
  snprintf(out, outLen, "%02d.%02d.%04d %02d:%02d",
           dt.date.date, dt.date.month, dt.date.year,
           dt.time.hours, dt.time.minutes);
}

/** VL-Bit (Reg 0x02 Bit 7): Uhr war stromlos, Zeit und Countdown unbrauchbar. */
bool rtcVoltLow() {
  uint8_t v = 0;
  if (!M5.In_I2C.readRegister(RTC_ADDR, RTC_REG_SECONDS, &v, 1, RTC_FREQ)) {
    return true;
  }
  return (v & 0x80) != 0;
}

int daysBetween(Ymd from, Ymd to) {
  auto toDays = [](const Ymd& y) -> int {
    int m = y.month;
    int yv = y.year;
    if (m <= 2) {
      yv -= 1;
      m += 12;
    }
    int era = (yv >= 0) ? yv / 400 - yv / 100 + yv / 4 : 0;
    return 365 * yv + era + (153 * m - 457) / 5 + y.day - 306;
  };
  return toDays(to) - toDays(from);
}

bool applyRtcDateTime(int year, int month, int day, int hour, int minute, int second) {
  m5::rtc_datetime_t dt = {};
  dt.date.year = year;
  dt.date.month = month;
  dt.date.date = day;
  dt.date.weekDay = 1;
  dt.time.hours = hour;
  dt.time.minutes = minute;
  dt.time.seconds = second;
  M5.Rtc.setDateTime(dt);
  return true;
}

uint32_t buildStampKey() {
  return (uint32_t)RTC_STAMP_YEAR * 100000000u
       + (uint32_t)RTC_STAMP_MONTH * 1000000u
       + (uint32_t)RTC_STAMP_DAY * 10000u
       + (uint32_t)RTC_STAMP_HOUR * 100u
       + (uint32_t)RTC_STAMP_MIN;
}

/** Plausibilität: tote/ungültige RTC → falsche Sleep-Dauer und stehender Tageszähler. */
bool rtcLooksValid() {
  m5::rtc_datetime_t dt;
  return readRtc(&dt) && rtcPlausible(dt);
}

/**
 * RTC setzen bei neuem Flash (rtc_stamp.h) oder wenn die RTC unplausibel ist.
 * Nicht bei jedem normalen Reboot überschreiben (Uhr soll weiterlaufen).
 */
bool ensureRtcFromBuildStamp() {
  uint32_t stampKey = buildStampKey();
  bool stampOk = prefs.getUInt(KEY_RTC_STAMP, 0u) == stampKey;
  // VL-Bit mitprüfen: nach Tiefentladung ist die Uhr zwar lesbar, aber falsch.
  if (stampOk && rtcLooksValid() && !rtcVoltLow()) {
    return false;
  }
  applyRtcDateTime(
    RTC_STAMP_YEAR,
    RTC_STAMP_MONTH,
    RTC_STAMP_DAY,
    RTC_STAMP_HOUR,
    RTC_STAMP_MIN,
    RTC_STAMP_SEC
  );
  prefs.putUInt(KEY_RTC_STAMP, stampKey);
  return true;
}

/** Erster Lauf: Starttag persistieren, damit Tage ab Flash/Reboot mitzählen. */
void ensureBaselineCleanDate() {
  if (prefs.isKey(KEY_LAST_DATE)) {
    return;
  }
  Ymd today;
  if (!calendarToday(&today)) {
    return;
  }
  uint32_t key = ymdToKey(today);
  if (key != 0) {
    prefs.putUInt(KEY_LAST_DATE, key);
  }
}

void loadState() {
  lastCleaner = prefs.getUChar(KEY_CLEANER, 0);
  if (lastCleaner >= CLEANER_COUNT) {
    lastCleaner = 0;
  }

  Ymd todayYmd;
  rtcUsable = calendarToday(&todayYmd);
  uint32_t today = rtcUsable ? ymdToKey(todayYmd) : 0;
  lastCleanYmd = prefs.getUInt(KEY_LAST_DATE, today);

  if (today == 0 || lastCleanYmd == 0) {
    // Ohne verlässliches Datum lieber den zuletzt gezeichneten Stand halten,
    // als aus Müllwerten einen dreistelligen Tageszähler zu rechnen.
    daysSinceClean = prefs.getInt(KEY_RENDER_DAYS, 0);
  } else {
    daysSinceClean = daysBetween(keyToYmd(lastCleanYmd), keyToYmd(today));
  }

  if (daysSinceClean < 0) {
    daysSinceClean = 0;
  }
  if (daysSinceClean > 999) {
    daysSinceClean = 999;
  }

  inverted = daysSinceClean > CLEAN_DAYS_INVERT;
}

bool saveCleaning(uint8_t cleanerIndex) {
  Ymd today;
  uint32_t key = calendarToday(&today) ? ymdToKey(today) : 0;
  if (key == 0) {
    // Ohne gültiges Datum nichts schreiben — ein kaputter Schlüssel in NVS
    // ist ohne Flash-Erase nicht mehr loszuwerden.
    return false;
  }
  lastCleanYmd = key;
  lastCleaner = cleanerIndex % CLEANER_COUNT;
  prefs.putUInt(KEY_LAST_DATE, lastCleanYmd);
  prefs.putUChar(KEY_CLEANER, lastCleaner);
  daysSinceClean = 0;
  inverted = false;
  screenDirty = true;
  return true;
}

float readBatteryVoltage() {
  if (M5.Power.getBatteryVoltage() > 0) {
    return M5.Power.getBatteryVoltage() / 1000.0f;
  }

  analogSetPinAttenuation(35, ADC_11db);
  esp_adc_cal_characteristics_t adcChars;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 3600, &adcChars);
  uint32_t mv = esp_adc_cal_raw_to_voltage(analogRead(35), &adcChars);
  return (float)mv * 25.1f / 5.1f / 1000.0f;
}

void migrateDiagnostics() {
  uint8_t version = prefs.getUChar(KEY_DIAG_VERSION, 0);
  if (version >= DIAG_VERSION) {
    return;
  }

  if (version < 1) {
    prefs.remove("boots");
    prefs.remove(KEY_VOLT_EMA);
  }
  // Ab Version 2 wird der Diagnose-Ringpuffer bewusst NIE gelöscht — er ist
  // die einzige Spur, die einen Freeze überlebt.
  prefs.putUChar(KEY_DIAG_VERSION, DIAG_VERSION);
}

/* --------------------------------------------------------------------------
 * Diagnose-Ringpuffer
 *
 * Ein Eintrag pro Boot in NVS. Zweimal geschrieben: einmal beim Start mit dem,
 * was dann bekannt ist, und einmal direkt vor dem Abschalten mit Sleep-Dauer
 * und Ergebnis. Bleibt das zweite Update aus, ist der Eintrag genau die Spur,
 * die zeigt, dass das Gerät den Sleep-Pfad nie erreicht hat.
 * ------------------------------------------------------------------------ */

void diagLoad() {
  size_t len = prefs.getBytes(KEY_DIAG_LOG, diagLog, sizeof(diagLog));
  if (len != sizeof(diagLog)) {
    memset(diagLog, 0, sizeof(diagLog));
  }
  diagHead = prefs.getUChar(KEY_DIAG_HEAD, 0) % DIAG_SLOTS;
}

BootRecord& diagCurrent() {
  return diagLog[diagHead];
}

/**
 * Endete der vorige Boot in einem verifizierten Power-Off, ist dieser Start
 * ein geplanter RTC-Wake.
 *
 * esp_sleep_get_wakeup_cause() taugt dafür nicht: nach echtem Power-Off ist
 * jeder Start ein Kaltstart und meldet UNDEFINED — genau deshalb lief bisher
 * jeder Weckvorgang die vollen 60 s statt des Heartbeat-Fensters.
 */
bool previousBootPoweredOff() {
  const BootRecord& prev = diagLog[(diagHead + DIAG_SLOTS - 1) % DIAG_SLOTS];
  return (prev.flags & DIAG_F_TIMER_OK) != 0
      && (prev.flags & DIAG_F_PWROFF_FAIL) == 0;
}

void diagFlush() {
  prefs.putBytes(KEY_DIAG_LOG, diagLog, sizeof(diagLog));
  prefs.putUChar(KEY_DIAG_HEAD, diagHead);
}

void diagBeginBoot(esp_sleep_wakeup_cause_t wakeCause) {
  uint8_t prevSeq = diagLog[diagHead].seq;
  diagHead = (diagHead + 1) % DIAG_SLOTS;

  BootRecord& r = diagCurrent();
  memset(&r, 0, sizeof(r));
  r.seq = prevSeq + 1;
  r.resetReason = (uint8_t)esp_reset_reason();
  r.wakeCause = (uint8_t)wakeCause;

  m5::rtc_datetime_t dt;
  if (readRtc(&dt)) {
    r.flags |= DIAG_F_RTC_READ;
    if (rtcPlausible(dt)) {
      r.flags |= DIAG_F_RTC_PLAUS;
      r.ymd = ymdToKey({dt.date.year, dt.date.month, dt.date.date});
      r.hm = (uint16_t)(dt.time.hours * 100 + dt.time.minutes);
    }
  }
  if (rtcVoltLow()) {
    r.flags |= DIAG_F_RTC_VLOW;
  }
  r.voltMv = (uint16_t)(readBatteryVoltage() * 1000.0f);

  diagFlush();

  Serial.printf("[boot] seq=%u reset=%u wake=%u rtc=%08lu %04u volt=%umV flags=0x%02X\n",
                r.seq, r.resetReason, r.wakeCause,
                (unsigned long)r.ymd, r.hm, r.voltMv, r.flags);
}

void applyBatteryFromEma(float ema) {
  float span = BAT_FULL_V - BAT_EMPTY_V;
  float remainingFrac = (ema - BAT_EMPTY_V) / span;
  if (remainingFrac < 0.0f) {
    remainingFrac = 0.0f;
  }
  if (remainingFrac > 1.0f) {
    remainingFrac = 1.0f;
  }

  batteryEstDays = remainingFrac * 90.0f;
  batteryLow = batteryEstDays <= BAT_WARN_DAYS;
}

/** Spannung lesen; NVS nur bei merklicher Änderung oder Mitternacht-Wake. */
void updateBatteryEstimate(bool persistToNvs) {
  float volts = readBatteryVoltage();
  float ema = prefs.getFloat(KEY_VOLT_EMA, volts);
  float newEma = (ema <= 0.1f) ? volts : (ema * 0.92f + volts * 0.08f);
  applyBatteryFromEma(newEma);

  float emaDelta = newEma > ema ? newEma - ema : ema - newEma;
  if (!persistToNvs && emaDelta < BAT_EMA_PERSIST_DELTA) {
    return;
  }

  prefs.putFloat(KEY_VOLT_EMA, newEma);
}

void formatBatteryDaysRemaining(char* out, size_t outLen) {
  int days = (int)(batteryEstDays + 0.5f);
  if (days < 0) {
    days = 0;
  }
  snprintf(out, outLen, "T-%d", days);
}

uint16_t inkColor() {
  return inverted ? TFT_WHITE : TFT_BLACK;
}

uint16_t paperColor() {
  return inverted ? TFT_BLACK : TFT_WHITE;
}

int grayStage() {
  if (daysSinceClean > CLEAN_DAYS_INVERT) {
    return -1;
  }
  if (daysSinceClean >= 6) {
    return 2;
  }
  if (daysSinceClean == 5) {
    return 1;
  }
  return 0;
}

void drawBackground() {
  if (inverted) {
    M5.Display.fillScreen(TFT_BLACK);
    return;
  }

  M5.Display.fillScreen(TFT_WHITE);
  int stage = grayStage();
  if (stage == 1) {
    for (int y = 0; y < SCREEN_H; y += 8) {
      M5.Display.drawFastHLine(0, y, SCREEN_W, TFT_BLACK);
    }
  } else if (stage == 2) {
    for (int y = 0; y < SCREEN_H; y += 4) {
      M5.Display.drawFastHLine(0, y, SCREEN_W, TFT_BLACK);
    }
  }
}

bool batteryIndicatorVisible() {
  return batteryLow || batteryDemoActive;
}

bool batteryAlarmMode() {
  return batteryIndicatorVisible() && batteryEstDays <= (float)BAT_ALARM_DAYS;
}

void moodIconLayout(int* cx, int* cy, int* size) {
  int maxW = SCREEN_W - (SCREEN_MARGIN * 2);
  int bottomUi = DAY_BAR_H + 4 + DAY_BAND_H + SCREEN_MARGIN;
  int topReserve = batteryAlarmMode() ? 46 : 0;
  int maxH = SCREEN_H - SCREEN_MARGIN - bottomUi - topReserve;
  int areaH = maxH;
  *size = (maxW < areaH) ? maxW : areaH;
  *cx = SCREEN_W / 2;
  *cy = SCREEN_MARGIN + topReserve + (areaH / 2);
}

void drawBoldCircle(int cx, int cy, int radius, uint16_t ink) {
  for (int offset = 0; offset < 6 && radius - offset > 0; offset++) {
    M5.Display.drawCircle(cx, cy, radius - offset, ink);
  }
}

void drawMouthCurve(int cx, int cy, int halfWidth, int depth, bool smile) {
  uint16_t ink = inkColor();

  for (int offset = -2; offset <= 2; offset++) {
    int baseY = cy + offset;
    int controlY = smile ? baseY + depth : baseY - depth;
    M5.Display.drawBezier(cx - halfWidth, baseY, cx, controlY, cx + halfWidth, baseY, ink);
  }
}

void drawMoodIconEyes(int cx, int cy, int size, int mood) {
  uint16_t ink = inkColor();
  int faceR = size / 2;
  int eyeY = cy - faceR / 5;
  int eyeX = (faceR * 2) / 5;
  int eyeR = faceR / 8;
  if (eyeR < 4) {
    eyeR = 4;
  }

  if (mood >= 2) {
    M5.Display.fillCircle(cx - eyeX, eyeY, eyeR, ink);
    M5.Display.fillCircle(cx + eyeX, eyeY, eyeR, ink);
    return;
  }

  if (mood == 1) {
    int w = eyeR * 2 + 2;
    M5.Display.fillRect(cx - eyeX - w / 2, eyeY - 1, w, 3, ink);
    M5.Display.fillRect(cx + eyeX - w / 2, eyeY - 1, w, 3, ink);
    return;
  }

  int browLen = faceR / 3;
  int browDrop = faceR / 9;
  M5.Display.drawLine(cx - eyeX - browLen, eyeY - browDrop, cx - eyeX + browLen / 2, eyeY + browDrop, ink);
  M5.Display.drawLine(cx - eyeX - browLen, eyeY - browDrop + 1, cx - eyeX + browLen / 2, eyeY + browDrop + 1, ink);
  M5.Display.drawLine(cx + eyeX + browLen, eyeY - browDrop, cx + eyeX - browLen / 2, eyeY + browDrop, ink);
  M5.Display.drawLine(cx + eyeX + browLen, eyeY - browDrop + 1, cx + eyeX - browLen / 2, eyeY + browDrop + 1, ink);
  M5.Display.fillCircle(cx - eyeX, eyeY + browDrop, eyeR - 1, ink);
  M5.Display.fillCircle(cx + eyeX, eyeY + browDrop, eyeR - 1, ink);
}

void drawMoodIconMouth(int cx, int cy, int size, int mood) {
  int faceR = size / 2;
  int mouthY = cy + faceR / 4;
  int mouthW = faceR / 2;

  if (mood >= 3) {
    drawMouthCurve(cx, mouthY, mouthW, faceR / 5, true);
  } else if (mood == 2) {
    drawMouthCurve(cx, mouthY, mouthW - 4, faceR / 8, true);
  } else if (mood == 1) {
    uint16_t ink = inkColor();
    for (int offset = -2; offset <= 2; offset++) {
      M5.Display.drawLine(cx - mouthW, mouthY + offset, cx + mouthW, mouthY + offset, ink);
    }
  } else {
    drawMouthCurve(cx, mouthY, mouthW - 4, faceR / 8, false);
  }
}

void drawMoodIcon(int mood) {
  int cx = 0;
  int cy = 0;
  int size = 0;
  moodIconLayout(&cx, &cy, &size);

  uint16_t ink = inkColor();
  uint16_t paper = paperColor();
  int faceR = size / 2;

  M5.Display.fillCircle(cx, cy, faceR, paper);
  drawBoldCircle(cx, cy, faceR, ink);
  drawMoodIconEyes(cx, cy, size, mood);
  drawMoodIconMouth(cx, cy, size, mood);
}

int smileyMood() {
  if (daysSinceClean > CLEAN_DAYS_INVERT) {
    return 0;
  }
  if (daysSinceClean >= CLEAN_DAYS_GRAY_START) {
    return 1;
  }
  if (daysSinceClean >= 3) {
    return 2;
  }
  return 3;
}

void formatDaysAgo(char* out, size_t outLen) {
  if (daysSinceClean == 0) {
    snprintf(out, outLen, "today");
  } else if (daysSinceClean == 1) {
    snprintf(out, outLen, "yesterday");
  } else {
    snprintf(out, outLen, "%d days ago", daysSinceClean);
  }
}

void drawThickRect(int x, int y, int w, int h, uint16_t ink, int thickness) {
  for (int t = 0; t < thickness; t++) {
    M5.Display.drawRect(x + t, y + t, w - (t * 2), h - (t * 2), ink);
  }
}

void drawBatteryGlyph(int x, int y, int bodyW, int bodyH, int tipW, int tipH, int innerX, int innerY,
                      int innerW, int innerH, int fillW, uint16_t ink, int stroke) {
  drawThickRect(x, y, bodyW, bodyH, ink, stroke);
  int tipY = y + (bodyH - tipH) / 2;
  drawThickRect(x + bodyW, tipY, tipW, tipH, ink, stroke);

  if (fillW > innerW) {
    fillW = innerW;
  }
  if (fillW > 0) {
    M5.Display.fillRect(innerX, innerY, fillW, innerH, ink);
  }
}

void drawAlarmMarks(int x, int y, int w, int h, uint16_t ink) {
  for (int i = -h; i < w; i += 6) {
    M5.Display.drawLine(x + i, y, x + i + h, y + h, ink);
    M5.Display.drawLine(x + i + 1, y, x + i + h + 1, y + h, ink);
  }
}

void drawSmallBatteryIndicator(uint16_t ink) {
  float scaleDays = batteryDemoActive ? (float)DEMO_BATTERY_START_DAYS : BAT_WARN_DAYS;
  int fillW = (int)(8.0f * batteryEstDays / scaleDays + 0.5f);
  if (fillW > 8) {
    fillW = 8;
  }
  if (fillW < 0) {
    fillW = 0;
  }

  drawBatteryGlyph(168, 4, 16, 8, 2, 4, 170, 6, 8, 4, fillW, ink, 1);
}

void drawAlarmBatteryIndicator(uint16_t ink) {
  static constexpr int BODY_W = 78;
  static constexpr int BODY_H = 34;
  static constexpr int TIP_W = 10;
  static constexpr int TIP_H = 18;
  static constexpr int INNER_PAD = 6;
  static constexpr int X = (SCREEN_W - BODY_W - TIP_W) / 2;
  static constexpr int Y = 6;
  static constexpr int INNER_W = BODY_W - (INNER_PAD * 2);
  static constexpr int INNER_H = BODY_H - (INNER_PAD * 2);

  int fillW = (int)(INNER_W * batteryEstDays / (float)BAT_ALARM_DAYS + 0.5f);
  if (fillW < 0) {
    fillW = 0;
  }

  drawBatteryGlyph(X, Y, BODY_W, BODY_H, TIP_W, TIP_H, X + INNER_PAD, Y + INNER_PAD,
                   INNER_W, INNER_H, fillW, ink, 3);

  if (batteryEstDays <= 3.0f) {
    drawAlarmMarks(X + INNER_PAD, Y + INNER_PAD, INNER_W, INNER_H, ink);
  }

  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(ink, paperColor());
  M5.Display.setTextSize(3);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("!", SCREEN_W / 2, Y + BODY_H + 14);
}

void drawBatteryIndicator() {
  if (!batteryIndicatorVisible()) {
    return;
  }

  uint16_t ink = inkColor();
  if (batteryAlarmMode()) {
    drawAlarmBatteryIndicator(ink);
  } else {
    drawSmallBatteryIndicator(ink);
  }
}

void drawCoffeeBean(int cx, int cy, bool filled) {
  uint16_t ink = inkColor();
  uint16_t paper = paperColor();
  int rx = BEAN_W / 2;
  int ry = BEAN_H / 2;

  if (filled) {
    M5.Display.fillEllipse(cx, cy, rx, ry, ink);
    M5.Display.drawLine(cx, cy - ry + 2, cx, cy + ry - 2, paper);
    M5.Display.drawBezier(cx, cy - ry / 2, cx - rx / 2, cy, cx, cy + ry / 2, paper);
  } else {
    M5.Display.drawEllipse(cx, cy, rx, ry, ink);
    M5.Display.drawEllipse(cx, cy, rx - 1, ry - 1, ink);
    M5.Display.drawLine(cx, cy - ry + 2, cx, cy + ry - 2, ink);
    M5.Display.drawBezier(cx, cy - ry / 2, cx - rx / 2, cy, cx, cy + ry / 2, ink);
  }
}

void drawDayIndicatorBar() {
  static constexpr int BAR_Y = SCREEN_H - DAY_BAND_H - 4 - DAY_BAR_H;
  static constexpr int SEGMENTS = CLEAN_DAYS_INVERT;
  static constexpr int GAP = 4;

  int step = (SCREEN_W - GAP * 2 - BEAN_W) / (SEGMENTS - 1);
  int beanY = BAR_Y + DAY_BAR_H / 2;

  for (int i = 0; i < SEGMENTS; i++) {
    int cx = GAP + BEAN_W / 2 + i * step;
    drawCoffeeBean(cx, beanY, daysSinceClean > i);
  }
}

void drawDayIndicatorLabel() {
  static constexpr int BAND_Y = SCREEN_H - DAY_BAND_H;

  char daysText[20];
  formatDaysAgo(daysText, sizeof(daysText));

  M5.Display.fillRect(0, BAND_Y, SCREEN_W, DAY_BAND_H, inkColor());
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(paperColor(), inkColor());
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(daysText, SCREEN_W / 2, BAND_Y + DAY_BAND_H / 2);
}

/** Letzte Boots am Gerät ablesbar machen — ohne USB, nach dem Freeze. */
void drawDiagScreen() {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);

  M5.Display.drawString("seq rst wk time  mV  fl", 2, 2);

  for (uint8_t i = 0; i < DIAG_SLOTS; i++) {
    // Neuester zuerst.
    const BootRecord& r = diagLog[(diagHead + DIAG_SLOTS - i) % DIAG_SLOTS];
    char line[40];
    snprintf(line, sizeof(line), "%3u %3u %2u %04u %4u %02X",
             r.seq, r.resetReason, r.wakeCause, r.hm, r.voltMv, r.flags);
    M5.Display.drawString(line, 2, 20 + i * 18);
  }

  M5.Display.drawString("1=rd 2=ok 4=vl 8=set", 2, 168);
  M5.Display.drawString("10=tmr 20=pof 40=dsl", 2, 184);
  commitDisplay();
}

void drawMainScreen() {
  drawBackground();

  drawMoodIcon(smileyMood());
  drawDayIndicatorBar();
  drawDayIndicatorLabel();
  drawBatteryIndicator();
}

static constexpr uint8_t SELECTION_ITEM_COUNT = CLEANER_COUNT + 1;
static constexpr uint8_t SELECTION_BACK_INDEX = CLEANER_COUNT;
static constexpr int SELECTION_TOP = 2;
static constexpr int SELECTION_BOTTOM_MARGIN = 2;
static constexpr int SELECTION_DATETIME_H = 16;
static constexpr int SELECTION_DATETIME_GAP = 2;
static constexpr int SELECTION_MENU_BOTTOM =
    SCREEN_H - SELECTION_BOTTOM_MARGIN - SELECTION_DATETIME_H - SELECTION_DATETIME_GAP;
static constexpr int SELECTION_MENU_H = SELECTION_MENU_BOTTOM - SELECTION_TOP;
static constexpr int SELECTION_ROW_H = SELECTION_MENU_H / SELECTION_ITEM_COUNT;
static constexpr int SELECTION_DATETIME_Y = SCREEN_H - SELECTION_BOTTOM_MARGIN;

int selectionRowCenterY(uint8_t index) {
  return SELECTION_TOP + ((index * 2 + 1) * SELECTION_MENU_H) / (SELECTION_ITEM_COUNT * 2);
}

const char* selectionLabel(uint8_t index) {
  if (index < CLEANER_COUNT) {
    return CLEANERS[index];
  }
  return "back";
}

void drawSelectionRow(uint8_t index, bool selected) {
  int rowY = selectionRowCenterY(index);
  int bandY = rowY - SELECTION_ROW_H / 2;

  M5.Display.fillRect(0, bandY, SCREEN_W, SELECTION_ROW_H, selected ? TFT_BLACK : TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(selected ? TFT_WHITE : TFT_BLACK, selected ? TFT_BLACK : TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(selectionLabel(index), SCREEN_W / 2, rowY);
}

void drawSelectionDateTime() {
  char dateTimeText[24];
  char daysText[8];
  formatRtcDateTime(dateTimeText, sizeof(dateTimeText));
  formatBatteryDaysRemaining(daysText, sizeof(daysText));

  static constexpr int FOOTER_PAD_X = 4;

  int footerY = SELECTION_MENU_BOTTOM;
  M5.Display.fillRect(0, footerY, SCREEN_W, SCREEN_H - footerY, TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextSize(1);

  M5.Display.setTextDatum(bottom_left);
  M5.Display.drawString(dateTimeText, FOOTER_PAD_X, SELECTION_DATETIME_Y);

  M5.Display.setTextDatum(bottom_right);
  M5.Display.drawString(daysText, SCREEN_W - FOOTER_PAD_X, SELECTION_DATETIME_Y);
}

/**
 * M5GFX' waitDisplay() gibt nach 1 s auf (_wait_busy Default-Timeout), ein
 * Full-Refresh des GDEW0154 braucht mehr. Danach in ein schlafendes Panel zu
 * schreiben oder den Strom zu kappen hinterlässt halbe Bilder — die dann als
 * "eingefroren" stehenbleiben.
 */
void waitPanelIdle(uint32_t timeoutMs) {
  delay(50);  // BUSY geht erst nach dem Latch des Refresh-Kommandos auf LOW
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    if (digitalRead(EPD_BUSY_PIN) == HIGH) {
      return;
    }
    feedWatchdog();
    delay(10);
  }
  Serial.printf("[epd] BUSY nach %ums noch aktiv\n", (unsigned)timeoutMs);
}

void commitDisplay() {
  M5.Display.endWrite();
  M5.Display.waitDisplay();
  waitPanelIdle(EPD_BUSY_TIMEOUT_MS);
}

void syncRenderedState() {
  lastRenderedDaysSinceClean = daysSinceClean;
  lastRenderedBatteryLow = batteryLow;
}

/** Ob E-Paper neu gezeichnet werden muss (NVS vs. aktueller Kalender/Akku). */
bool hasDisplayStateChanged() {
  int storedDays = prefs.getInt(KEY_RENDER_DAYS, -999);
  bool storedBat = prefs.getBool(KEY_RENDER_BAT, false);
  return storedDays != daysSinceClean || storedBat != batteryLow;
}

void saveDisplayState() {
  prefs.putInt(KEY_RENDER_DAYS, daysSinceClean);
  prefs.putBool(KEY_RENDER_BAT, batteryLow);
  syncRenderedState();
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

void refreshMainScreen(epd_mode_t mode) {
  feedWatchdog();
  M5.Display.setEpdMode(mode);
  M5.Display.startWrite();
  drawMainScreen();
  commitDisplay();
  screenDirty = false;
  saveDisplayState();
  feedWatchdog();
}

bool isTimerWake(esp_sleep_wakeup_cause_t cause) {
  return cause == ESP_SLEEP_WAKEUP_TIMER;
}

bool isButtonWake(esp_sleep_wakeup_cause_t cause) {
  return cause == ESP_SLEEP_WAKEUP_GPIO
      || cause == ESP_SLEEP_WAKEUP_EXT0
      || cause == ESP_SLEEP_WAKEUP_EXT1;
}

void setupWatchdog() {
  esp_task_wdt_config_t cfg = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  // reconfigure falls WDT schon läuft (Arduino-Start), sonst init
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    esp_task_wdt_init(&cfg);
  }
  esp_task_wdt_add(NULL);
}

/** Warten bis Tasten losgelassen — verhindert sofortiges Re-Wake aus Sleep. */
void waitButtonsReleased(uint32_t maxMs) {
  uint32_t start = millis();
  while ((millis() - start) < maxMs) {
    M5.update();
    if (!userActive()) {
      return;
    }
    feedWatchdog();
    delay(10);
  }
}

void applyPreviewDay(int day) {
  daysSinceClean = day;
  inverted = day > CLEAN_DAYS_INVERT;
}

void runDayPreviewDemo() {
  for (int day = 0; day < DEMO_DAY_COUNT; day++) {
    applyPreviewDay(day);
    refreshMainScreen(epd_mode_t::epd_fast);
    delay(DEMO_DAY_MS);
  }
}

void runBatteryPreviewDemo() {
  batteryDemoActive = true;
  daysSinceClean = 0;
  inverted = false;

  for (int day = DEMO_BATTERY_START_DAYS; day >= 0; day--) {
    batteryEstDays = (float)day;
    batteryLow = day <= (int)BAT_WARN_DAYS;
    refreshMainScreen(epd_mode_t::epd_fast);
    delay(DEMO_BATTERY_MS);
  }

  batteryDemoActive = false;
}

void enterSelectionScreen() {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  for (uint8_t i = 0; i < SELECTION_ITEM_COUNT; i++) {
    drawSelectionRow(i, i == picker);
  }
  drawSelectionDateTime();
  commitDisplay();
}

void updateSelectionPicker(uint8_t previousPicker) {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  drawSelectionRow(previousPicker, false);
  drawSelectionRow(picker, true);
  commitDisplay();
}

/**
 * CoreInk-Wahlhebel (M5 Docs PinMap):
 *   G37 Dial Up  = BtnA  → Menü: vorheriger Eintrag
 *   G38 Dial Mid = BtnB  → Menü: bestätigen
 *   G39 Dial Down= BtnC  → Menü: nächster Eintrag
 *   G5  UserBtn  = BtnEXT
 *   G27 PWR      = BtnPWR — einziger manueller Wake aus Power-Off (2 s halten)
 */
bool dialClicked() {
  return M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked()
      || M5.BtnEXT.wasClicked();
}

bool userActive() {
  return M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()
      || M5.BtnPWR.isPressed() || M5.BtnEXT.isPressed();
}

/** Sekunden bis Mitternacht (Kalender-Rollover). */
uint32_t secondsUntilNextDailyWake() {
  auto dt = M5.Rtc.getDateTime();
  int secondsNow = dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
  int secondsTarget = DAILY_ROLLOVER_HOUR * 3600;
  int delta = secondsTarget - secondsNow;
  if (delta <= 0) {
    delta += 86400;
  }
  return (uint32_t)delta;
}

/**
 * Countdown für BM8563: min(Rest bis Mitternacht, MAX_SLEEP_SEC),
 * geclampt auf den Chip-Timer (max. 255 min).
 */
uint32_t secondsUntilNextWake() {
  uint32_t toDaily = secondsUntilNextDailyWake();
  uint32_t sec = toDaily;
  if (sec > MAX_SLEEP_SEC) {
    sec = MAX_SLEEP_SEC;
  }
  if (sec < MIN_SLEEP_SEC) {
    sec = MIN_SLEEP_SEC;
  }
  return sec;
}

/**
 * BM8563-Countdown stellen und per Register-Rücklesen verifizieren.
 *
 * Ohne diese Prüfung ist der Power-Off eine Wette: greift der I2C-Write nicht,
 * schaltet sich das Gerät ab und nichts weckt es je wieder — bis jemand PWR
 * zwei Sekunden hält. Genau das sieht von außen wie ein Freeze aus.
 */
bool armRtcWakeTimer(uint32_t sec) {
  M5.Rtc.disableIRQ();
  M5.Rtc.clearIRQ();

  int applied = M5.Rtc.setAlarmIRQ((int)sec);
  if (applied <= 0) {
    Serial.printf("[rtc] setAlarmIRQ(%u) -> %d\n", (unsigned)sec, applied);
    return false;
  }

  uint8_t ctl = 0;
  uint8_t cnt = 0;
  uint8_t st2 = 0;
  bool read = M5.In_I2C.readRegister(RTC_ADDR, RTC_REG_TIMER_CTL, &ctl, 1, RTC_FREQ)
           && M5.In_I2C.readRegister(RTC_ADDR, RTC_REG_TIMER_CNT, &cnt, 1, RTC_FREQ)
           && M5.In_I2C.readRegister(RTC_ADDR, RTC_REG_STATUS2, &st2, 1, RTC_FREQ);

  // 0x0E Bit7 = Timer läuft, 0x0F = Countdown-Wert, 0x01 Bit0 = TIE.
  bool armed = read && (ctl & 0x80) != 0 && cnt > 0 && (st2 & 0x01) != 0;
  Serial.printf("[rtc] sec=%u applied=%d ctl=0x%02X cnt=%u st2=0x%02X -> %s\n",
                (unsigned)sec, applied, ctl, cnt, st2, armed ? "armed" : "FEHLER");
  return armed;
}

/**
 * Rückfallebene, wenn der Power-Off nicht greift oder der RTC-Wecker nicht
 * verifiziert werden konnte.
 *
 * Bewusst KEIN esp_deep_sleep_start(): im Deep-Sleep gibt der ESP32 GPIO12
 * (POWER_HOLD) frei, der CoreInk verliert die Versorgung — und ohne armierten
 * BM8563-Timer weckt danach nichts mehr. Genau so wird aus einem Sleep ein
 * dauerhafter Brick. Stattdessen Light-Sleep in Häppchen: POWER_HOLD bleibt
 * getrieben, zwischendurch wird der WDT bedient und geprüft, ob die RTC
 * zurück ist.
 */
void fallbackChunkedSleep(uint32_t totalSec) {
  // Über die Sleep-Dauer läuft der Task-WDT weiter und schlägt beim Aufwachen
  // sofort zu — für die Dauer der Häppchen abmelden.
  esp_task_wdt_delete(NULL);

  uint32_t slept = 0;
  while (slept < totalSec) {
    uint32_t chunk = totalSec - slept;
    if (chunk > SLEEP_CHUNK_SEC) {
      chunk = SLEEP_CHUNK_SEC;
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)chunk * 1000000ULL);
    // PWR bleibt bedienbar, damit das Gerät im Notbetrieb nicht tot wirkt.
    esp_sleep_enable_ext0_wakeup(PWR_BTN_PIN, 0);
    esp_light_sleep_start();
    slept += chunk;

    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
      break;  // Tastendruck
    }
    if (rtcLooksValid() && !rtcVoltLow()) {
      break;  // RTC ist zurück — nächster Durchgang kann wieder abschalten
    }
  }

  esp_task_wdt_add(NULL);
  feedWatchdog();
  Serial.printf("[sleep] Häppchen-Sleep beendet nach %us\n", (unsigned)slept);
}

/**
 * Idle = Versorgung trennen, BM8563 weckt durch erneutes Anlegen.
 *
 * - E-Paper hält das Bild ohne Strom
 * - Manuell wecken: PWR ~2 s halten (Wahlhebel ist im Off tot — Hardware!)
 * - Kehrt nur zurück, wenn der Power-Hold nicht getrennt hat (z. B. am USB)
 */
void enterIdleSleep() {
  feedWatchdog();
  M5.Power.setLed(0);
  waitButtonsReleased(2000);

  bool rtcOk = rtcLooksValid() && !rtcVoltLow();
  uint32_t sec = rtcOk ? secondsUntilNextWake() : RTC_FALLBACK_SLEEP_SEC;
  bool armed = rtcOk && armRtcWakeTimer(sec);

  BootRecord& r = diagCurrent();
  r.sleepSec10 = (uint16_t)(sec / 10);
  if (armed) {
    r.flags |= DIAG_F_TIMER_OK;
  }
  diagFlush();

  waitPanelIdle(EPD_BUSY_TIMEOUT_MS);
  M5.Display.sleep();
  delay(50);
  Serial.flush();

  if (armed) {
    // Einmal LOW und LOW lassen. M5.Power.timerSleep() pulst hier fünfmal
    // LOW/HIGH und endet HIGH — auf dem latch-basierten CoreInk bleibt das
    // Gerät dann an und landet in einem stundenlangen Light-Sleep.
    pinMode(POWER_HOLD_PIN, OUTPUT);
    digitalWrite(POWER_HOLD_PIN, LOW);
    delay(500);
  }

  // Hier nur, wenn die Versorgung nicht wegging oder der Timer nicht steht.
  if (armed) {
    r.flags |= DIAG_F_PWROFF_FAIL;
  }
  r.flags |= DIAG_F_CHUNKED;
  diagFlush();
  Serial.printf("[sleep] kein Power-Off (armed=%d) — Häppchen-Sleep %us\n",
                armed ? 1 : 0, (unsigned)sec);
  Serial.flush();

  fallbackChunkedSleep(sec);
}

void setup() {
  // Nach Power-Off/RTC-Wake ist das ein Kaltstart (cause oft UNDEFINED).
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  auto cfg = M5.config();
  // Ohne Log war jeder bisherige Freeze-Fix ein Blindschuss.
  cfg.serial_baudrate = 115200;
  // Wir zeichnen ohnehin immer komplett neu — der Clear spart einen Full-Refresh.
  cfg.clear_display = false;
  // Nichts davon wird benutzt, kostet aber Boot-Zeit und Strom.
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  cfg.output_power = false;
  M5.begin(cfg);

  // Vor allem anderen abfragen: wer per PWR geweckt hat, hält jetzt noch.
  M5.update();
  bool pwrHeldAtBoot = M5.BtnPWR.isPressed();

  M5.Power.setLed(0);

  // RTC-IRQ vom Weckvorgang quittieren (sonst Soft-Loops).
  M5.Rtc.clearIRQ();

  setupWatchdog();

  prefs.begin(NAMESPACE, false);
  migrateDiagnostics();
  diagLoad();
  // Erst protokollieren, dann korrigieren: ensureRtcFromBuildStamp() loescht
  // beim Setzen der Uhr das VL-Bit, das wir gerade festhalten wollen.
  diagBeginBoot(wakeCause);
  if (ensureRtcFromBuildStamp()) {
    diagCurrent().flags |= DIAG_F_RTC_RESET;
    diagFlush();
  }
  ensureBaselineCleanDate();
  loadState();
  Serial.printf("[state] days=%d lastClean=%lu rtcUsable=%d\n",
                daysSinceClean, (unsigned long)lastCleanYmd, rtcUsable ? 1 : 0);
  // Jeder Kaltstart: EMA sichern (Power-Off verliert RAM).
  updateBatteryEstimate(true);

  picker = 0;
  awakeLimitMs = AWAKE_MS;
  M5.BtnB.setHoldThresh(DIAG_HOLD_MS);

  if (DEMO_DAY_PREVIEW) {
    runDayPreviewDemo();
    loadState();
  }

  if (DEMO_BATTERY_PREVIEW) {
    runBatteryPreviewDemo();
    updateBatteryEstimate(true);
  }

  if (!DEMO_DAY_PREVIEW && !DEMO_BATTERY_PREVIEW) {
    // IMMER neu zeichnen nach M5.begin / Power-On / Flash.
    // Skip bei "gleicher Tag" ließ das Panel weiß (init/flash löscht den
    // Framebuffer; E-Ink hält nur, wenn wir gar nicht re-initen — tun wir aber).
    bool changed = hasDisplayStateChanged();
    // Nicht epd_quality: dessen Doppel-Refresh im Panel-Treiber wartet nach dem
    // Refresh-Kommando nur 320 ms und schreibt dann weiter SPI, während BUSY
    // noch aktiv ist — das erzeugt halbe Bilder.
    refreshMainScreen(epd_mode_t::epd_fast);

    // Volles Fenster bei Tageswechsel, manuellem Wake und nach Flash/Absturz.
    // Stiller RTC-Wake bekommt nur den Heartbeat; greift der Nutzer doch zu,
    // verlängert userActive() in loop() wieder auf AWAKE_MS.
    bool manualWake = pwrHeldAtBoot || isButtonWake(wakeCause);
    bool plannedWake = previousBootPoweredOff();
    awakeLimitMs = (changed || manualWake || !plannedWake)
        ? (uint32_t)AWAKE_MS
        : (uint32_t)HEARTBEAT_AWAKE_MS;
    Serial.printf("[wake] changed=%d manual=%d planned=%d awake=%ums\n",
                  changed ? 1 : 0, manualWake ? 1 : 0, plannedWake ? 1 : 0,
                  (unsigned)awakeLimitMs);
  }

  // LED kurz: Gerät ist wach und bedienbar (Wahlhebel wirkt nur JETZT).
  M5.Power.setLed(128);
  delay(80);
  M5.Power.setLed(0);
  feedWatchdog();
}

void loop() {
  static uint32_t awakeStart = millis();
  static uint32_t bootStart = millis();

  feedWatchdog();
  M5.update();

  if (userActive()) {
    awakeStart = millis();
    // Sobald der Nutzer interagiert: volle Wachzeit, nicht nur Heartbeat-Fenster.
    awakeLimitMs = AWAKE_MS;
  }

  if (showingDiag) {
    if (dialClicked()) {
      showingDiag = false;
      screenDirty = true;
    }
  } else if (!selecting && M5.BtnB.wasHold()) {
    showingDiag = true;
    drawDiagScreen();
  } else {
    bool enteredSelection = false;
    if (!selecting && dialClicked()) {
      selecting = true;
      picker = 0;
      enteredSelection = true;
      enterSelectionScreen();
    }

    if (selecting && !enteredSelection) {
      // Dial Up = vorherige Zeile, Dial Down = nächste, Mid = OK
      if (M5.BtnA.wasClicked()) {
        uint8_t previousPicker = picker;
        picker = (picker + SELECTION_ITEM_COUNT - 1) % SELECTION_ITEM_COUNT;
        updateSelectionPicker(previousPicker);
      }
      if (M5.BtnC.wasClicked()) {
        uint8_t previousPicker = picker;
        picker = (picker + 1) % SELECTION_ITEM_COUNT;
        updateSelectionPicker(previousPicker);
      }
      if (M5.BtnB.wasClicked()) {
        selecting = false;
        awakeStart = millis();
        if (picker != SELECTION_BACK_INDEX && !saveCleaning(picker)) {
          Serial.printf("[state] Reinigung nicht gespeichert — RTC unbrauchbar\n");
        }
        refreshMainScreen(epd_mode_t::epd_fastest);
      }
    }

    if (!selecting && screenDirty) {
      refreshMainScreen(epd_mode_t::epd_fastest);
    }
  }

  bool windowOver = (millis() - awakeStart) >= awakeLimitMs;
  // Harte Obergrenze unabhängig von den Tasten: ein klemmender Eingang darf
  // das Gerät nicht bis zum leeren Akku wachhalten.
  bool hardLimit = (millis() - bootStart) >= MAX_AWAKE_MS;

  if (windowOver || hardLimit) {
    if (hardLimit && !windowOver) {
      Serial.printf("[loop] Hard-Limit %ums erreicht — Tasten klemmen?\n",
                    (unsigned)MAX_AWAKE_MS);
    }
    if (showingDiag) {
      showingDiag = false;
      screenDirty = true;
    }
    if (selecting) {
      selecting = false;
      screenDirty = true;
    }
    if (screenDirty) {
      refreshMainScreen(epd_mode_t::epd_fastest);
    }
    // Kehrt nur zurueck, wenn der Power-Off nicht greifen konnte. Dann ist das
    // Datum inzwischen weitergelaufen — Zustand neu einlesen und neu zeichnen.
    enterIdleSleep();
    loadState();
    screenDirty = true;
    awakeStart = millis();
    bootStart = millis();
    awakeLimitMs = AWAKE_MS;
  }

  delay(selecting ? 1 : 20);
}
