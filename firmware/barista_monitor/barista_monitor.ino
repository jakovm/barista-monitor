#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>

static constexpr const char* NAMESPACE = "coffee";
static constexpr const char* KEY_LAST_DATE = "lastYmd";
static constexpr const char* KEY_CLEANER = "cleaner";
static constexpr const char* KEY_RTC_INIT = "rtcInit";
static constexpr const char* KEY_BOOT_COUNT = "boots";
static constexpr const char* KEY_VOLT_EMA = "voltEma";

static constexpr const char* CLEANERS[] = {"Jakov", "Nina", "Guest"};
static constexpr uint8_t CLEANER_COUNT = 3;

static constexpr uint8_t CLEAN_DAYS_GRAY_START = 5;
static constexpr uint8_t CLEAN_DAYS_INVERT = 7;
static constexpr int SCREEN_W = 200;
static constexpr int SCREEN_H = 200;
static constexpr int SCREEN_MARGIN = 4;
static constexpr int DAY_BAND_H = 32;
static constexpr int DAY_BAR_H = 10;
static constexpr uint16_t AWAKE_MS = 30000;
static constexpr float BAT_WARN_DAYS = 10.0f;
static constexpr float BAT_FULL_V = 4.10f;
static constexpr float BAT_EMPTY_V = 3.35f;

Preferences prefs;
bool selecting = false;
uint8_t picker = 0;
bool screenDirty = true;
int daysSinceClean = 0;
uint32_t lastCleanYmd = 0;
uint8_t lastCleaner = 0;
bool inverted = false;
bool batteryLow = false;

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

Ymd rtcToday() {
  auto dt = M5.Rtc.getDateTime();
  return {dt.date.year, dt.date.month, dt.date.date};
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

void initRtcOnce() {
  if (prefs.getBool(KEY_RTC_INIT, false)) {
    return;
  }

  const char* dateStr = __DATE__;
  const char* timeStr = __TIME__;
  const char* months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

  char monthName[4] = {0};
  int day = 1;
  int year = 2026;
  sscanf(dateStr, "%3s %d %d", monthName, &day, &year);

  int month = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(monthName, months[i], 3) == 0) {
      month = i + 1;
      break;
    }
  }

  int hour = 0;
  int minute = 0;
  int second = 0;
  sscanf(timeStr, "%d:%d:%d", &hour, &minute, &second);

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
}

void loadState() {
  uint32_t today = ymdToKey(rtcToday());
  lastCleanYmd = prefs.getUInt(KEY_LAST_DATE, today);
  lastCleaner = prefs.getUChar(KEY_CLEANER, 0);
  if (lastCleaner >= CLEANER_COUNT) {
    lastCleaner = 0;
  }

  daysSinceClean = daysBetween(keyToYmd(lastCleanYmd), keyToYmd(today));
  if (daysSinceClean < 0) {
    daysSinceClean = 0;
  }

  inverted = daysSinceClean >= CLEAN_DAYS_INVERT;
}

void saveCleaning(uint8_t cleanerIndex) {
  Ymd today = rtcToday();
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

  float estDays = remainingFrac * 90.0f;
  batteryLow = estDays <= BAT_WARN_DAYS;
}

uint16_t inkColor() {
  return inverted ? TFT_WHITE : TFT_BLACK;
}

uint16_t paperColor() {
  return inverted ? TFT_BLACK : TFT_WHITE;
}

int grayStage() {
  if (daysSinceClean >= CLEAN_DAYS_INVERT) {
    return -1;
  }
  if (daysSinceClean == 6) {
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

void moodIconLayout(int* cx, int* cy, int* size) {
  int maxW = SCREEN_W - (SCREEN_MARGIN * 2);
  int bottomUi = DAY_BAR_H + 4 + DAY_BAND_H + SCREEN_MARGIN;
  int maxH = SCREEN_H - SCREEN_MARGIN - bottomUi;
  int areaH = SCREEN_H - SCREEN_MARGIN - bottomUi;
  *size = (maxW < maxH) ? maxW : maxH;
  *cx = SCREEN_W / 2;
  *cy = SCREEN_MARGIN + (areaH / 2);
}

void drawBoldCircle(int cx, int cy, int radius, uint16_t ink) {
  M5.Display.drawCircle(cx, cy, radius, ink);
  if (radius > 2) {
    M5.Display.drawCircle(cx, cy, radius - 1, ink);
  }
}

void drawIconArc(int cx, int cy, int radius, bool smile) {
  uint16_t ink = inkColor();
  uint16_t paper = paperColor();
  int clipY = smile ? cy - (radius / 2) : cy + (radius / 2);

  M5.Display.drawCircle(cx, cy, radius, ink);
  M5.Display.drawCircle(cx, clipY, radius, paper);
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
  int mouthR = faceR / 3;

  if (mood >= 3) {
    drawIconArc(cx, mouthY - mouthR / 4, mouthR, true);
  } else if (mood == 2) {
    drawIconArc(cx, mouthY, mouthR - 2, true);
  } else if (mood == 1) {
    M5.Display.drawLine(cx - mouthR, mouthY, cx + mouthR, mouthY, inkColor());
    M5.Display.drawLine(cx - mouthR, mouthY + 1, cx + mouthR, mouthY + 1, inkColor());
  } else {
    drawIconArc(cx, mouthY + mouthR / 4, mouthR - 2, false);
    int corner = mouthR / 2;
    M5.Display.drawLine(cx - corner, mouthY + mouthR, cx - 4, mouthY + mouthR / 2, inkColor());
    M5.Display.drawLine(cx + corner, mouthY + mouthR, cx + 4, mouthY + mouthR / 2, inkColor());
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
  if (daysSinceClean >= CLEAN_DAYS_INVERT) {
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

void drawBatteryIndicator() {
  if (!batteryLow) {
    return;
  }

  uint16_t ink = inkColor();
  M5.Display.drawRect(168, 4, 16, 8, ink);
  M5.Display.fillRect(184, 6, 2, 4, ink);
  M5.Display.fillRect(170, 6, 8, 4, ink);
}

void drawDayIndicatorBar() {
  static constexpr int BAR_Y = SCREEN_H - DAY_BAND_H - 4 - DAY_BAR_H;
  static constexpr int SEGMENTS = CLEAN_DAYS_INVERT;
  static constexpr int GAP = 2;

  uint16_t ink = inkColor();
  uint16_t paper = paperColor();
  int segW = (SCREEN_W - GAP * (SEGMENTS + 1)) / SEGMENTS;

  for (int i = 0; i < SEGMENTS; i++) {
    int x = GAP + i * (segW + GAP);
    if (daysSinceClean > i) {
      M5.Display.fillRect(x, BAR_Y, segW, DAY_BAR_H, ink);
    } else {
      M5.Display.drawRect(x, BAR_Y, segW, DAY_BAR_H, ink);
      M5.Display.fillRect(x + 1, BAR_Y + 1, segW - 2, DAY_BAR_H - 2, paper);
    }
  }
}

void drawDayIndicatorLabel() {
  static constexpr int BAND_Y = SCREEN_H - DAY_BAND_H;

  char daysText[20];
  formatDaysAgo(daysText, sizeof(daysText));

  M5.Display.fillRect(0, BAND_Y, SCREEN_W, DAY_BAND_H, inkColor());
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(paperColor(), inkColor());
  M5.Display.setTextSize(1);
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

static constexpr int SELECTION_ROW_Y[] = {36, 100, 164};
static constexpr int SELECTION_ROW_H = 56;

void drawSelectionRow(uint8_t index, bool selected) {
  int bandY = SELECTION_ROW_Y[index] - SELECTION_ROW_H / 2;

  M5.Display.fillRect(0, bandY, 200, SELECTION_ROW_H, selected ? TFT_BLACK : TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(selected ? TFT_WHITE : TFT_BLACK, selected ? TFT_BLACK : TFT_WHITE);
  M5.Display.setTextSize(selected ? 2 : 1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(CLEANERS[index], 100, SELECTION_ROW_Y[index]);
}

void commitDisplay() {
  M5.Display.endWrite();
}

void refreshMainScreen(epd_mode_t mode) {
  M5.Display.setEpdMode(mode);
  M5.Display.startWrite();
  drawMainScreen();
  commitDisplay();
  screenDirty = false;
}

bool wokeFromDailyTimer() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

void enterSelectionScreen() {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  for (uint8_t i = 0; i < CLEANER_COUNT; i++) {
    drawSelectionRow(i, i == picker);
  }
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

uint32_t secondsUntilNextDailyWake() {
  auto dt = M5.Rtc.getDateTime();
  int hour = dt.time.hours;
  int minute = dt.time.minutes;
  int second = dt.time.seconds;

  int targetHour = 7;
  int secondsNow = hour * 3600 + minute * 60 + second;
  int secondsTarget = targetHour * 3600;
  int delta = secondsTarget - secondsNow;
  if (delta <= 300) {
    delta += 86400;
  }
  return (uint32_t)delta;
}

void enterSleep() {
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Power.setLed(0);
  WiFi.mode(WIFI_OFF);

  uint32_t sleepSec = secondsUntilNextDailyWake();
  if (sleepSec < 1800u) {
    sleepSec = 1800u;
  }
  M5.Power.timerSleep((int)sleepSec);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  M5.Power.setLed(0);

  prefs.begin(NAMESPACE, false);
  initRtcOnce();
  loadState();
  updateBatteryEstimate();

  picker = 0;
  refreshMainScreen(wokeFromDailyTimer() ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);

  M5.Speaker.setVolume(0);
}

void loop() {
  static uint32_t awakeStart = millis();

  M5.update();

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
      picker = (picker + CLEANER_COUNT - 1) % CLEANER_COUNT;
      updateSelectionPicker(previousPicker);
    }
    if (M5.BtnC.wasClicked()) {
      uint8_t previousPicker = picker;
      picker = (picker + 1) % CLEANER_COUNT;
      updateSelectionPicker(previousPicker);
    }
    if (M5.BtnB.wasClicked()) {
      saveCleaning(picker);
      selecting = false;
      awakeStart = millis();
      refreshMainScreen(epd_mode_t::epd_fastest);
    }
  }

  if (!selecting && screenDirty) {
    refreshMainScreen(epd_mode_t::epd_fastest);
  }

  if (!selecting && (millis() - awakeStart) >= AWAKE_MS) {
    enterSleep();
  }

  delay(selecting ? 1 : 50);
}