#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <string.h>
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
static constexpr uint8_t DIAG_VERSION = 1;

/** Feste Slots. Umbenennen behält den Zähler, Leeren löscht ihn. */
static constexpr uint8_t MAX_CLEANERS = 4;
/** AsciiFont8x16 Größe 2: 9 Zeichen lassen rechts noch Platz für den Zähler. */
static constexpr uint8_t CLEANER_NAME_MAX = 9;
static constexpr uint8_t LIST_MAX = 6;
/** AsciiFont kennt keine Umlaute. */
static const char EDIT_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-";

enum class Mode : uint8_t { Main, Root, Clean, Stats, Names, Edit };

enum ListTag : uint8_t {
  TAG_BACK = 32,
  TAG_ADD = 33,
  TAG_CLEAN = 34,
  TAG_STATS = 35,
  TAG_NAMES = 36,
};

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
/** Task-WDT: bei hängendem E-Paper-Refresh Neustart statt Dauer-Freeze. */
static constexpr uint32_t WDT_TIMEOUT_MS = 30000;
/**
 * CoreInk-Idle = echter Power-Off + BM8563-Timer (M5.Power.timerSleep).
 * BM8563 Countdown: max. 255 Minuten → Cap 4 h.
 * Sleep = min(Sekunden bis Mitternacht, MAX_SLEEP_SEC).
 *
 * NICHT ESP light/deep sleep als Hauptpfad:
 * - Light-Sleep kann hängen (WDT pausiert → tot)
 * - Deep-Sleep: nur 1 EXT0-Pin; Wahlhebel-Down/Right weckt nicht
 * - GPIO19 (RTC-INT) ist kein RTC-GPIO → weckt Deep-Sleep nicht (USB/Forum)
 * Offiziell (M5 Docs): RTC + Power-Hold; Aufwachen per PWR oder RTC.
 */
static constexpr uint32_t MAX_SLEEP_SEC = 255u * 60u;
static constexpr uint32_t MIN_SLEEP_SEC = 5;
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
Mode mode = Mode::Main;
uint8_t picker = 0;
bool screenDirty = true;
char cleanerNames[MAX_CLEANERS][CLEANER_NAME_MAX + 1];
uint16_t cleanerCounts[MAX_CLEANERS];
char listLabels[LIST_MAX][CLEANER_NAME_MAX + 1];
uint8_t listTags[LIST_MAX];
uint8_t listCount = 0;
char editBuf[CLEANER_NAME_MAX + 1];
char editNote[12];
uint8_t editLen = 0;
uint8_t editCursor = 0;
uint8_t editSlot = 0;
uint8_t editPicker = 0;
bool editSpin = false;
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

struct Ymd {
  int year;
  int month;
  int day;
};

uint32_t ymdToKey(const Ymd& ymd) {
  return (uint32_t)ymd.year * 10000u + (uint32_t)ymd.month * 100u + (uint32_t)ymd.day;
}

Ymd keyToYmd(uint32_t key) {
  return {
    (int)(key / 10000u),
    (int)((key / 100u) % 100u),
    (int)(key % 100u),
  };
}

/** Kalendertag laut RTC (Wechsel um Mitternacht). */
Ymd calendarToday() {
  auto dt = M5.Rtc.getDateTime();
  return {dt.date.year, dt.date.month, dt.date.date};
}

void formatRtcDateTime(char* out, size_t outLen) {
  auto dt = M5.Rtc.getDateTime();
  snprintf(out, outLen, "%02d.%02d.%04d %02d:%02d",
           dt.date.date, dt.date.month, dt.date.year,
           dt.time.hours, dt.time.minutes);
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
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2024 || dt.date.year > 2036) {
    return false;
  }
  if (dt.date.month < 1 || dt.date.month > 12) {
    return false;
  }
  if (dt.date.date < 1 || dt.date.date > 31) {
    return false;
  }
  if (dt.time.hours > 23 || dt.time.minutes > 59 || dt.time.seconds > 59) {
    return false;
  }
  return true;
}

/**
 * RTC setzen bei neuem Flash (rtc_stamp.h) oder wenn die RTC unplausibel ist.
 * Nicht bei jedem normalen Reboot überschreiben (Uhr soll weiterlaufen).
 */
void ensureRtcFromBuildStamp() {
  uint32_t stampKey = buildStampKey();
  bool stampOk = prefs.getUInt(KEY_RTC_STAMP, 0u) == stampKey;
  if (stampOk && rtcLooksValid()) {
    return;
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
}

/** Erster Lauf: Starttag persistieren, damit Tage ab Flash/Reboot mitzählen. */
void ensureBaselineCleanDate() {
  if (prefs.isKey(KEY_LAST_DATE)) {
    return;
  }
  prefs.putUInt(KEY_LAST_DATE, ymdToKey(calendarToday()));
}

uint8_t countActive() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    if (cleanerNames[i][0] != 0) {
      n++;
    }
  }
  return n;
}

uint8_t firstActiveSlot() {
  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    if (cleanerNames[i][0] != 0) {
      return i;
    }
  }
  return 0;
}

int firstFreeSlot() {
  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    if (cleanerNames[i][0] == 0) {
      return i;
    }
  }
  return -1;
}

void slotNameKey(uint8_t slot, char* out) {
  snprintf(out, 4, "n%u", slot);
}

void slotCountKey(uint8_t slot, char* out) {
  snprintf(out, 4, "k%u", slot);
}

void sanitizeName(char* name) {
  size_t w = 0;
  for (size_t r = 0; name[r] != 0 && w < CLEANER_NAME_MAX; r++) {
    char c = name[r];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '-';
    if (ok) {
      name[w++] = c;
    }
  }
  name[w] = 0;
}

void persistSlot(uint8_t slot) {
  char key[4];
  slotNameKey(slot, key);
  prefs.putString(key, cleanerNames[slot]);
  slotCountKey(slot, key);
  prefs.putUShort(key, cleanerCounts[slot]);
}

void seedDefaultCleaners() {
  const char* defaults[MAX_CLEANERS] = {"Jakov", "Nina", "Guest", ""};
  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    strncpy(cleanerNames[i], defaults[i], CLEANER_NAME_MAX);
    cleanerNames[i][CLEANER_NAME_MAX] = 0;
    cleanerCounts[i] = 0;
    persistSlot(i);
  }
}

void loadCleaners() {
  if (!prefs.isKey("n0")) {
    seedDefaultCleaners();
    return;
  }

  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    char key[4];
    cleanerNames[i][0] = 0;
    slotNameKey(i, key);
    prefs.getString(key, cleanerNames[i], CLEANER_NAME_MAX + 1);
    cleanerNames[i][CLEANER_NAME_MAX] = 0;
    sanitizeName(cleanerNames[i]);
    slotCountKey(i, key);
    cleanerCounts[i] = prefs.getUShort(key, 0);
  }

  if (countActive() == 0) {
    seedDefaultCleaners();
  }
}

void loadState() {
  uint32_t today = ymdToKey(calendarToday());
  lastCleanYmd = prefs.getUInt(KEY_LAST_DATE, today);
  lastCleaner = prefs.getUChar(KEY_CLEANER, 0);
  if (lastCleaner >= MAX_CLEANERS || cleanerNames[lastCleaner][0] == 0) {
    lastCleaner = firstActiveSlot();
  }

  daysSinceClean = daysBetween(keyToYmd(lastCleanYmd), keyToYmd(today));
  if (daysSinceClean < 0) {
    daysSinceClean = 0;
  }

  inverted = daysSinceClean > CLEAN_DAYS_INVERT;
}

void saveCleaning(uint8_t slot) {
  if (slot >= MAX_CLEANERS || cleanerNames[slot][0] == 0) {
    return;
  }

  Ymd today = calendarToday();
  lastCleanYmd = ymdToKey(today);
  lastCleaner = slot;
  prefs.putUInt(KEY_LAST_DATE, lastCleanYmd);
  prefs.putUChar(KEY_CLEANER, lastCleaner);
  if (cleanerCounts[slot] < 65535) {
    cleanerCounts[slot]++;
  }
  char key[4];
  slotCountKey(slot, key);
  prefs.putUShort(key, cleanerCounts[slot]);
  daysSinceClean = 0;
  inverted = false;
  screenDirty = true;
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
  if (prefs.getUChar(KEY_DIAG_VERSION, 0) >= DIAG_VERSION) {
    return;
  }

  prefs.remove("boots");
  prefs.remove(KEY_VOLT_EMA);
  prefs.putUChar(KEY_DIAG_VERSION, DIAG_VERSION);
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

void drawMainScreen() {
  drawBackground();

  drawMoodIcon(smileyMood());
  drawDayIndicatorBar();
  drawDayIndicatorLabel();
  drawBatteryIndicator();
}

static constexpr int LIST_TOP = 2;
static constexpr int LIST_DATETIME_H = 16;
static constexpr int LIST_DATETIME_Y = SCREEN_H - 2;

void commitDisplay() {
  M5.Display.endWrite();
  M5.Display.waitDisplay();
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

void refreshMainScreen(epd_mode_t epdMode) {
  feedWatchdog();
  M5.Display.setEpdMode(epdMode);
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

void addListItem(const char* label, uint8_t tag) {
  if (listCount >= LIST_MAX) {
    return;
  }
  strncpy(listLabels[listCount], label, CLEANER_NAME_MAX);
  listLabels[listCount][CLEANER_NAME_MAX] = 0;
  listTags[listCount] = tag;
  listCount++;
}

void buildList() {
  listCount = 0;
  if (mode == Mode::Root) {
    addListItem("clean", TAG_CLEAN);
    addListItem("stats", TAG_STATS);
    addListItem("names", TAG_NAMES);
    addListItem("back", TAG_BACK);
    return;
  }

  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    if (cleanerNames[i][0] != 0) {
      addListItem(cleanerNames[i], i);
    }
  }
  if (mode == Mode::Names && countActive() < MAX_CLEANERS) {
    addListItem("add", TAG_ADD);
  }
  addListItem("back", TAG_BACK);
}

int listMenuBottom() {
  if (listCount <= 4) {
    return SCREEN_H - 2 - LIST_DATETIME_H - 2;
  }
  return SCREEN_H - 2;
}

int listMenuH() {
  return listMenuBottom() - LIST_TOP;
}

int listRowCenterY(uint8_t index) {
  return LIST_TOP + ((int)index * 2 + 1) * listMenuH() / ((int)listCount * 2);
}

int listRowH() {
  if (listCount == 0) {
    return 1;
  }
  return listMenuH() / (int)listCount;
}

void drawListFooter() {
  char dateTimeText[24];
  char daysText[8];
  formatRtcDateTime(dateTimeText, sizeof(dateTimeText));
  formatBatteryDaysRemaining(daysText, sizeof(daysText));

  int footerY = listMenuBottom();
  M5.Display.fillRect(0, footerY, SCREEN_W, SCREEN_H - footerY, TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(bottom_left);
  M5.Display.drawString(dateTimeText, 4, LIST_DATETIME_Y);
  M5.Display.setTextDatum(bottom_right);
  M5.Display.drawString(daysText, SCREEN_W - 4, LIST_DATETIME_Y);
}

void drawListRow(uint8_t index, bool selected) {
  int rowY = listRowCenterY(index);
  int bandH = listRowH();
  int bandY = rowY - bandH / 2;
  uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
  uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;

  M5.Display.fillRect(0, bandY, SCREEN_W, bandH, bg);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextSize(2);

  if (mode == Mode::Stats && listTags[index] < MAX_CLEANERS) {
    uint8_t slot = listTags[index];
    char countBuf[8];
    snprintf(countBuf, sizeof(countBuf), "%u", (unsigned)cleanerCounts[slot]);
    bool wideCount = strlen(cleanerNames[slot]) <= 7;
    M5.Display.setTextDatum(middle_left);
    M5.Display.drawString(cleanerNames[slot], 6, rowY);
    M5.Display.setTextSize(wideCount ? 2 : 1);
    M5.Display.setTextDatum(middle_right);
    M5.Display.drawString(countBuf, SCREEN_W - 6, rowY);
    return;
  }

  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(listLabels[index], SCREEN_W / 2, rowY);
}

void enterList(Mode next) {
  mode = next;
  editSpin = false;
  picker = 0;
  buildList();
  if (listCount == 0) {
    return;
  }

  feedWatchdog();
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  for (uint8_t i = 0; i < listCount; i++) {
    drawListRow(i, i == picker);
  }
  if (listCount <= 4) {
    drawListFooter();
  }
  commitDisplay();
  feedWatchdog();
}

void updateListPicker(uint8_t previousPicker) {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  drawListRow(previousPicker, false);
  drawListRow(picker, true);
  commitDisplay();
}

void showMain() {
  mode = Mode::Main;
  editSpin = false;
  refreshMainScreen(epd_mode_t::epd_fastest);
}

void movePicker(int dir, uint8_t count, void (*redraw)(uint8_t)) {
  if (count == 0) {
    return;
  }
  uint8_t previous = picker;
  int next = (int)picker + dir;
  if (next < 0) {
    next = (int)count - 1;
  }
  if (next >= (int)count) {
    next = 0;
  }
  picker = (uint8_t)next;
  redraw(previous);
}

static constexpr int EDIT_PREVIEW_H = 48;
static constexpr int EDIT_HINT_Y = 48;
static constexpr int EDIT_HINT_H = 18;
static constexpr int EDIT_MENU_TOP_IDLE = 50;
static constexpr int EDIT_MENU_TOP_SPIN = 68;
static constexpr int EDIT_MENU_BOTTOM = 198;
static constexpr uint8_t EDIT_ROWS = 5;

int editMenuTop() {
  return editSpin ? EDIT_MENU_TOP_SPIN : EDIT_MENU_TOP_IDLE;
}

int editMenuH() {
  return EDIT_MENU_BOTTOM - editMenuTop();
}

int editRowCenterY(uint8_t index) {
  return editMenuTop() + ((int)index * 2 + 1) * editMenuH() / (EDIT_ROWS * 2);
}

int editRowH() {
  return editMenuH() / EDIT_ROWS;
}

char editCursorChar() {
  if (editCursor < editLen) {
    return editBuf[editCursor];
  }
  return '_';
}

void drawEditPreview() {
  M5.Display.fillRect(0, 0, SCREEN_W, EDIT_PREVIEW_H, TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);

  if (editNote[0] != 0) {
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.drawString(editNote, SCREEN_W / 2, EDIT_PREVIEW_H / 2);
    return;
  }

  bool phantom = editLen < CLEANER_NAME_MAX && (editLen == 0 || editCursor >= editLen);
  uint8_t cells = editLen + (phantom ? 1 : 0);
  if (cells == 0) {
    cells = 1;
  }

  static constexpr int PITCH = 20;
  static constexpr int CELL_W = 18;
  static constexpr int CELL_H = 40;
  int x = (SCREEN_W - (int)cells * PITCH) / 2;
  int y = (EDIT_PREVIEW_H - CELL_H) / 2;

  for (uint8_t i = 0; i < cells; i++) {
    bool onCursor = i == editCursor;
    char ch = (i < editLen) ? editBuf[i] : '_';
    uint16_t bg = onCursor ? TFT_BLACK : TFT_WHITE;
    uint16_t fg = onCursor ? TFT_WHITE : TFT_BLACK;
    char tmp[2] = {ch, 0};
    M5.Display.fillRect(x, y, CELL_W, CELL_H, bg);
    M5.Display.drawRect(x, y, CELL_W, CELL_H, fg);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(tmp, x + CELL_W / 2, y + CELL_H / 2);
    x += PITCH;
  }
}

void drawEditHint() {
  M5.Display.fillRect(0, EDIT_HINT_Y, SCREEN_W, EDIT_HINT_H, TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("mid next   hold mid back", SCREEN_W / 2, EDIT_HINT_Y + EDIT_HINT_H / 2);
}

void drawEditRow(uint8_t index, bool selected) {
  int rowY = editRowCenterY(index);
  int bandH = editRowH();
  int bandY = rowY - bandH / 2;
  uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
  uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
  char label[16];

  if (index == 0) {
    snprintf(label, sizeof(label), "change %c", editCursorChar());
  } else {
    const char* labels[] = {"change", "next", "delete", "save", "back"};
    snprintf(label, sizeof(label), "%s", labels[index]);
  }

  M5.Display.fillRect(0, bandY, SCREEN_W, bandH, bg);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(label, SCREEN_W / 2, rowY);
}

void redrawEdit() {
  feedWatchdog();
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  drawEditPreview();
  if (editSpin) {
    drawEditHint();
  }
  for (uint8_t i = 0; i < EDIT_ROWS; i++) {
    drawEditRow(i, i == editPicker);
  }
  commitDisplay();
  feedWatchdog();
}

void refreshEditDynamic() {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  drawEditPreview();
  drawEditRow(0, editPicker == 0);
  commitDisplay();
}

void beginEdit(uint8_t slot) {
  editSlot = slot;
  editNote[0] = 0;
  editSpin = false;
  editPicker = 0;
  strncpy(editBuf, cleanerNames[slot], CLEANER_NAME_MAX);
  editBuf[CLEANER_NAME_MAX] = 0;
  editLen = (uint8_t)strlen(editBuf);
  editCursor = 0;
  mode = Mode::Edit;
  redrawEdit();
}

void stepEditChar(int dir) {
  editNote[0] = 0;
  int charsetLen = (int)strlen(EDIT_CHARSET);
  bool append = editCursor >= editLen;
  int idx = 0;
  if (!append) {
    const char* found = strchr(EDIT_CHARSET, editBuf[editCursor]);
    idx = found ? (int)(found - EDIT_CHARSET) : 0;
  } else {
    idx = dir > 0 ? -1 : charsetLen;
  }
  idx += dir;
  if (idx < 0) {
    idx = charsetLen - 1;
  }
  if (idx >= charsetLen) {
    idx = 0;
  }
  if (append) {
    if (editLen >= CLEANER_NAME_MAX) {
      return;
    }
    editBuf[editLen] = EDIT_CHARSET[idx];
    editLen++;
    editBuf[editLen] = 0;
    editCursor = editLen - 1;
    return;
  }
  editBuf[editCursor] = EDIT_CHARSET[idx];
}

void moveEditCursor(int dir) {
  editNote[0] = 0;
  if (editLen == 0) {
    editCursor = 0;
    return;
  }
  int maxCursor = (editLen < CLEANER_NAME_MAX) ? (int)editLen : (int)editLen - 1;
  int next = (int)editCursor + dir;
  if (next < 0) {
    next = maxCursor;
  }
  if (next > maxCursor) {
    next = 0;
  }
  editCursor = (uint8_t)next;
}

void deleteEditChar() {
  editNote[0] = 0;
  if (editLen == 0) {
    return;
  }
  uint8_t idx = (editCursor >= editLen) ? editLen - 1 : editCursor;
  memmove(&editBuf[idx], &editBuf[idx + 1], editLen - idx);
  editLen--;
  editBuf[editLen] = 0;
  if (editCursor > editLen) {
    editCursor = editLen;
  }
}

void setEditNote(const char* note) {
  strncpy(editNote, note, sizeof(editNote) - 1);
  editNote[sizeof(editNote) - 1] = 0;
  refreshEditDynamic();
}

void saveEdit() {
  if (editLen == 0) {
    if (cleanerNames[editSlot][0] == 0) {
      enterList(Mode::Names);
      return;
    }
    if (countActive() <= 1) {
      setEditNote("keep one");
      return;
    }
    cleanerNames[editSlot][0] = 0;
    cleanerCounts[editSlot] = 0;
    persistSlot(editSlot);
    if (lastCleaner == editSlot) {
      lastCleaner = firstActiveSlot();
      prefs.putUChar(KEY_CLEANER, lastCleaner);
    }
    enterList(Mode::Names);
    return;
  }

  editBuf[editLen] = 0;
  for (uint8_t i = 0; i < MAX_CLEANERS; i++) {
    if (i != editSlot && cleanerNames[i][0] != 0 && strcmp(cleanerNames[i], editBuf) == 0) {
      setEditNote("taken");
      return;
    }
  }

  strncpy(cleanerNames[editSlot], editBuf, CLEANER_NAME_MAX);
  cleanerNames[editSlot][CLEANER_NAME_MAX] = 0;
  persistSlot(editSlot);
  enterList(Mode::Names);
}

void updateEditPicker(uint8_t previousPicker) {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  drawEditRow(previousPicker, false);
  drawEditRow(editPicker, true);
  commitDisplay();
}

void activateEditRow() {
  if (editPicker == 0) {
    editSpin = true;
    editNote[0] = 0;
    redrawEdit();
    return;
  }
  if (editPicker == 1) {
    moveEditCursor(1);
    refreshEditDynamic();
    return;
  }
  if (editPicker == 2) {
    deleteEditChar();
    refreshEditDynamic();
    return;
  }
  if (editPicker == 3) {
    saveEdit();
    return;
  }
  enterList(Mode::Names);
}

bool takeRepeat(m5::Button_Class& btn, uint32_t& nextMs) {
  uint32_t now = millis();
  if (btn.wasPressed()) {
    nextMs = now + 420;
    return true;
  }
  if (btn.isPressed() && (int32_t)(now - nextMs) >= 0) {
    nextMs = now + 130;
    return true;
  }
  return false;
}

void handleEditInput() {
  static uint32_t upRepeatMs = 0;
  static uint32_t downRepeatMs = 0;

  if (editSpin) {
    bool changed = false;
    if (takeRepeat(M5.BtnA, upRepeatMs)) {
      stepEditChar(-1);
      changed = true;
    }
    if (takeRepeat(M5.BtnC, downRepeatMs)) {
      stepEditChar(1);
      changed = true;
    }
    if (M5.BtnB.wasClicked()) {
      moveEditCursor(1);
      changed = true;
    }
    if (changed) {
      refreshEditDynamic();
    }
    if (M5.BtnB.wasHold() || M5.BtnEXT.wasClicked()) {
      editSpin = false;
      redrawEdit();
    }
    return;
  }

  if (M5.BtnA.wasClicked()) {
    uint8_t previous = editPicker;
    editPicker = (editPicker + EDIT_ROWS - 1) % EDIT_ROWS;
    updateEditPicker(previous);
  }
  if (M5.BtnC.wasClicked()) {
    uint8_t previous = editPicker;
    editPicker = (editPicker + 1) % EDIT_ROWS;
    updateEditPicker(previous);
  }
  if (M5.BtnB.wasClicked()) {
    activateEditRow();
  }
  if (M5.BtnEXT.wasClicked()) {
    enterList(Mode::Names);
  }
}

void confirmList() {
  uint8_t tag = listTags[picker];
  if (mode == Mode::Root) {
    if (tag == TAG_CLEAN) {
      enterList(Mode::Clean);
    } else if (tag == TAG_STATS) {
      enterList(Mode::Stats);
    } else if (tag == TAG_NAMES) {
      enterList(Mode::Names);
    } else {
      showMain();
    }
    return;
  }
  if (mode == Mode::Clean) {
    if (tag == TAG_BACK) {
      enterList(Mode::Root);
    } else {
      saveCleaning(tag);
      showMain();
    }
    return;
  }
  if (mode == Mode::Stats) {
    if (tag == TAG_BACK) {
      enterList(Mode::Root);
    }
    return;
  }
  if (tag == TAG_BACK) {
    enterList(Mode::Root);
  } else if (tag == TAG_ADD) {
    int slot = firstFreeSlot();
    if (slot >= 0) {
      beginEdit((uint8_t)slot);
    }
  } else {
    beginEdit(tag);
  }
}

void handleListInput() {
  if (M5.BtnA.wasClicked()) {
    movePicker(-1, listCount, updateListPicker);
  }
  if (M5.BtnC.wasClicked()) {
    movePicker(1, listCount, updateListPicker);
  }
  if (M5.BtnB.wasClicked()) {
    confirmList();
  }
  if (M5.BtnEXT.wasClicked()) {
    if (mode == Mode::Root) {
      showMain();
    } else {
      enterList(Mode::Root);
    }
  }
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
 * Idle = Power-Off + RTC-Wecker (offizieller CoreInk-Weg).
 *
 * - E-Paper hält das Bild ohne Strom
 * - BM8563 weckt durch erneutes Anlegen der Versorgung (nicht GPIO19-Deep-Sleep)
 * - Manuell wecken: PWR ~2 s halten (Wahlhebel ist im Off tot — Hardware!)
 * - Kehrt normalerweise nicht zurück
 */
void enterIdleSleep() {
  feedWatchdog();
  M5.Power.setLed(0);
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  waitButtonsReleased(2000);

  if (!rtcLooksValid()) {
    // Ohne gültige RTC kein sinnvoller Timer — kurzer Soft-Reset, dann erneut.
    delay(100);
    esp_restart();
  }

  uint32_t sec = secondsUntilNextWake();

  // Offizielle API: RTC-Countdown + Power-Hold-Abschaltung (Akku).
  // timerSleep löscht IRQs, setzt BM8563-Timer, schaltet HOLD aus.
  // USB: Power bleibt oft an → M5 fällt intern auf Light-Sleep/Restart zurück.
  M5.Power.timerSleep((int)sec);

  // Falls timerSleep zurückkehrt (z. B. USB ohne echten Power-Off):
  // ESP-Timer + Deep-Sleep als Notnagel, dann sauberer Boot.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  delay(50);
  esp_deep_sleep_start();
}

void setup() {
  // Nach Power-Off/RTC-Wake ist das ein Kaltstart (cause oft UNDEFINED).
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  auto cfg = M5.config();
  M5.begin(cfg);

  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  M5.Power.setLed(0);

  // RTC-IRQ vom Weckvorgang quittieren (sonst Soft-Loops).
  M5.Rtc.clearIRQ();

  setupWatchdog();

  prefs.begin(NAMESPACE, false);
  ensureRtcFromBuildStamp();
  ensureBaselineCleanDate();
  migrateDiagnostics();
  loadCleaners();
  loadState();
  // Jeder Kaltstart: EMA sichern (Power-Off verliert RAM).
  updateBatteryEstimate(true);

  picker = 0;
  awakeLimitMs = AWAKE_MS;

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
    refreshMainScreen(epd_mode_t::epd_quality);
    // Voller Wachfenster bei Tageswechsel oder manuellem Wake; Heartbeat kürzer.
    awakeLimitMs = (changed || isButtonWake(wakeCause) || wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED)
        ? (uint32_t)AWAKE_MS
        : (uint32_t)HEARTBEAT_AWAKE_MS;
    // Kaltstart nach Flash/PWR ist UNDEFINED → volle Wachzeit (Menü bedienbar).
    if (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED) {
      awakeLimitMs = AWAKE_MS;
    }
  }

  M5.Speaker.setVolume(0);
  // LED kurz: Gerät ist wach und bedienbar (Wahlhebel wirkt nur JETZT).
  M5.Power.setLed(128);
  delay(80);
  M5.Power.setLed(0);
  feedWatchdog();
}

void loop() {
  static uint32_t awakeStart = millis();

  feedWatchdog();
  M5.update();

  if (userActive()) {
    awakeStart = millis();
    // Sobald der Nutzer interagiert: volle Wachzeit, nicht nur Heartbeat-Fenster.
    awakeLimitMs = AWAKE_MS;
  }

  if (mode == Mode::Main) {
    if (dialClicked()) {
      enterList(Mode::Root);
    }
  } else if (mode == Mode::Edit) {
    handleEditInput();
  } else {
    handleListInput();
  }

  if (mode == Mode::Main && screenDirty) {
    refreshMainScreen(epd_mode_t::epd_fastest);
  }

  if ((millis() - awakeStart) >= awakeLimitMs) {
    if (mode != Mode::Main) {
      mode = Mode::Main;
      editSpin = false;
      screenDirty = true;
      refreshMainScreen(epd_mode_t::epd_fastest);
    }
    enterIdleSleep();
    // Nur erreichbar, wenn Power-Off fehlschlug (z. B. USB): neu wach.
    awakeStart = millis();
    awakeLimitMs = AWAKE_MS;
  }

  delay(mode == Mode::Main ? 20 : 1);
}
