#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>
#include "rtc_stamp.h"

static constexpr const char* NAMESPACE = "coffee";
static constexpr const char* KEY_LAST_DATE = "lastYmd";
static constexpr const char* KEY_CLEANER = "cleaner";
static constexpr const char* KEY_RTC_INIT = "rtcInit";
static constexpr const char* KEY_BOOT_COUNT = "boots";
static constexpr const char* KEY_VOLT_EMA = "voltEma";
static constexpr const char* KEY_DIAG_VERSION = "diagVer";
static constexpr uint8_t DIAG_VERSION = 1;

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
static constexpr uint16_t AWAKE_MS = 60000;
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
/** Logischer Tageswechsel und täglicher Display-Refresh (Light-Sleep-Timer). */
static constexpr int LOGICAL_DAY_START_HOUR = 4;

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

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int month) {
  static constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return lengths[month - 1];
}

Ymd previousDay(Ymd ymd) {
  if (ymd.day > 1) {
    return {ymd.year, ymd.month, ymd.day - 1};
  }
  if (ymd.month > 1) {
    int month = ymd.month - 1;
    return {ymd.year, month, daysInMonth(ymd.year, month)};
  }
  int year = ymd.year - 1;
  return {year, 12, 31};
}

Ymd logicalToday() {
  auto dt = M5.Rtc.getDateTime();
  Ymd today = {dt.date.year, dt.date.month, dt.date.date};
  if (dt.time.hours < LOGICAL_DAY_START_HOUR) {
    return previousDay(today);
  }
  return today;
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
  prefs.putBool(KEY_RTC_INIT, true);
  return true;
}

/** RTC aus Host-Lokalzeit beim letzten flash/sync (rtc_stamp.h). */
void applyRtcFromBuildStamp() {
  applyRtcDateTime(
    RTC_STAMP_YEAR,
    RTC_STAMP_MONTH,
    RTC_STAMP_DAY,
    RTC_STAMP_HOUR,
    RTC_STAMP_MIN,
    RTC_STAMP_SEC
  );
}

/** Erster Lauf: Starttag persistieren, damit Tage ab Flash/Reboot mitzählen. */
void ensureBaselineCleanDate() {
  if (prefs.isKey(KEY_LAST_DATE)) {
    return;
  }
  prefs.putUInt(KEY_LAST_DATE, ymdToKey(logicalToday()));
}

void loadState() {
  uint32_t today = ymdToKey(logicalToday());
  lastCleanYmd = prefs.getUInt(KEY_LAST_DATE, today);
  lastCleaner = prefs.getUChar(KEY_CLEANER, 0);
  if (lastCleaner >= CLEANER_COUNT) {
    lastCleaner = 0;
  }

  daysSinceClean = daysBetween(keyToYmd(lastCleanYmd), keyToYmd(today));
  if (daysSinceClean < 0) {
    daysSinceClean = 0;
  }

  inverted = daysSinceClean > CLEAN_DAYS_INVERT;
}

void saveCleaning(uint8_t cleanerIndex) {
  Ymd today = logicalToday();
  lastCleanYmd = ymdToKey(today);
  lastCleaner = cleanerIndex % CLEANER_COUNT;
  prefs.putUInt(KEY_LAST_DATE, lastCleanYmd);
  prefs.putUChar(KEY_CLEANER, lastCleaner);
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

  prefs.remove(KEY_BOOT_COUNT);
  prefs.remove(KEY_VOLT_EMA);
  prefs.putUChar(KEY_DIAG_VERSION, DIAG_VERSION);
}

void updateBatteryEstimate() {
  float volts = readBatteryVoltage();
  uint32_t boots = prefs.getUInt(KEY_BOOT_COUNT, 0u) + 1u;
  prefs.putUInt(KEY_BOOT_COUNT, boots);

  float ema = prefs.getFloat(KEY_VOLT_EMA, volts);
  ema = (ema <= 0.1f) ? volts : (ema * 0.92f + volts * 0.08f);
  prefs.putFloat(KEY_VOLT_EMA, ema);

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

void commitDisplay() {
  M5.Display.endWrite();
}

void syncRenderedState() {
  lastRenderedDaysSinceClean = daysSinceClean;
  lastRenderedBatteryLow = batteryLow;
}

bool displayNeedsRefresh() {
  return daysSinceClean != lastRenderedDaysSinceClean || batteryLow != lastRenderedBatteryLow;
}

void refreshMainScreen(epd_mode_t mode) {
  M5.Display.setEpdMode(mode);
  M5.Display.startWrite();
  drawMainScreen();
  commitDisplay();
  screenDirty = false;
  syncRenderedState();
}

bool wokeFromDailyTimer() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
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
  updateBatteryEstimate();
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

bool dialClicked() {
  return M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked();
}

bool userActive() {
  return M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()
      || M5.BtnPWR.isPressed() || M5.BtnEXT.isPressed();
}

void enableButtonWakeup() {
  gpio_wakeup_enable(GPIO_NUM_37, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_38, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_39, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_27, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_5, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

/** Sekunden bis zum nächsten Weckruf um LOGICAL_DAY_START_HOUR (04:00). */
uint32_t secondsUntilNextDailyWake() {
  auto dt = M5.Rtc.getDateTime();
  int secondsNow = dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
  int secondsTarget = LOGICAL_DAY_START_HOUR * 3600;
  int delta = secondsTarget - secondsNow;
  if (delta <= 0) {
    delta += 86400;
  }
  return (uint32_t)delta;
}

void enterIdleSleep() {
  M5.Display.sleep();
  M5.Power.setLed(0);
  WiFi.mode(WIFI_OFF);

  while (true) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    enableButtonWakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)secondsUntilNextDailyWake() * 1000000ULL);

    esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    // 04:00: Zähler neu laden und Display immer aktualisieren (auch ohne Zustandsänderung).
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
      loadState();
      updateBatteryEstimate();
      if (!selecting) {
        M5.Display.wakeup();
        refreshMainScreen(epd_mode_t::epd_fast);
        M5.Display.sleep();
      }
      continue;
    }

    if (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_EXT0) {
      loadState();
      updateBatteryEstimate();
      M5.Display.wakeup();
      delay(30);
      M5.update();
      if (!selecting && displayNeedsRefresh()) {
        refreshMainScreen(epd_mode_t::epd_fast);
      }
      return;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  M5.Power.setLed(0);

  prefs.begin(NAMESPACE, false);
  applyRtcFromBuildStamp();
  ensureBaselineCleanDate();
  migrateDiagnostics();
  loadState();
  updateBatteryEstimate();

  picker = 0;

  if (DEMO_DAY_PREVIEW) {
    runDayPreviewDemo();
    loadState();
  }

  if (DEMO_BATTERY_PREVIEW) {
    runBatteryPreviewDemo();
    updateBatteryEstimate();
  }

  refreshMainScreen(wokeFromDailyTimer() ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);

  M5.Speaker.setVolume(0);
}

void loop() {
  static uint32_t awakeStart = millis();

  M5.update();

  if (userActive()) {
    awakeStart = millis();
  }

  bool enteredSelection = false;
  if (!selecting && dialClicked()) {
    selecting = true;
    picker = 0;
    enteredSelection = true;
    enterSelectionScreen();
  }

  if (selecting && !enteredSelection) {
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
      if (picker == SELECTION_BACK_INDEX) {
        refreshMainScreen(epd_mode_t::epd_fastest);
      } else {
        saveCleaning(picker);
        refreshMainScreen(epd_mode_t::epd_fastest);
      }
    }
  }

  if (!selecting && screenDirty) {
    refreshMainScreen(epd_mode_t::epd_fastest);
  }

  if (!selecting && (millis() - awakeStart) >= AWAKE_MS) {
    enterIdleSleep();
    awakeStart = millis();
  }

  delay(selecting ? 1 : 20);
}