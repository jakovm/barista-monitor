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
static constexpr uint16_t AWAKE_MS = 120000;
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
    for (int y = 0; y < 200; y += 6) {
      M5.Display.drawFastHLine(0, y, 200, TFT_BLACK);
    }
  } else if (stage == 2) {
    for (int y = 0; y < 200; y += 3) {
      M5.Display.drawFastHLine(0, y, 200, TFT_BLACK);
    }
  }
}

void drawSmiley(int cx, int cy, int mood) {
  uint16_t ink = inkColor();
  uint16_t paper = paperColor();

  int eyeY = cy - 12;
  int mouthY = cy + 14;

  if (mood >= 2) {
    M5.Display.fillCircle(cx - 16, eyeY, 5, ink);
    M5.Display.fillCircle(cx + 16, eyeY, 5, ink);
  } else {
    M5.Display.drawLine(cx - 20, eyeY - 2, cx - 10, eyeY + 5, ink);
    M5.Display.drawLine(cx - 10, eyeY + 5, cx - 20, eyeY + 5, ink);
    M5.Display.drawLine(cx + 20, eyeY - 2, cx + 10, eyeY + 5, ink);
    M5.Display.drawLine(cx + 10, eyeY + 5, cx + 20, eyeY + 5, ink);
  }

  if (mood >= 3) {
    M5.Display.drawCircle(cx, mouthY, 18, ink);
    M5.Display.drawCircle(cx, mouthY - 7, 18, paper);
  } else if (mood == 2) {
    M5.Display.drawCircle(cx, mouthY - 2, 14, ink);
    M5.Display.drawCircle(cx, mouthY - 9, 14, paper);
  } else if (mood == 1) {
    M5.Display.drawLine(cx - 16, mouthY, cx + 16, mouthY, ink);
  } else {
    M5.Display.drawCircle(cx, mouthY + 5, 14, ink);
    M5.Display.drawCircle(cx, mouthY - 9, 14, paper);
    M5.Display.drawLine(cx - 12, mouthY + 18, cx + 2, mouthY + 11, ink);
    M5.Display.drawLine(cx + 12, mouthY + 18, cx - 2, mouthY + 11, ink);
  }
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

void drawMainScreen() {
  drawBackground();

  drawSmiley(100, 88, smileyMood());

  char daysText[20];
  formatDaysAgo(daysText, sizeof(daysText));

  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(inkColor(), paperColor());
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(daysText, 100, 168);

  drawBatteryIndicator();
}

void drawSelectionScreen() {
  static constexpr int ROW_Y[] = {36, 100, 164};

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);

  for (uint8_t i = 0; i < CLEANER_COUNT; i++) {
    if (i == picker) {
      M5.Display.fillRect(0, ROW_Y[i] - 28, 200, 56, TFT_BLACK);
      M5.Display.setTextFont(&fonts::FreeSansBold18pt7b);
      M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Display.setTextSize(1);
    } else {
      M5.Display.setTextFont(&fonts::FreeSansBold12pt7b);
      M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
      M5.Display.setTextSize(1);
    }
    M5.Display.drawString(CLEANERS[i], 100, ROW_Y[i]);
  }
}

void refreshDisplay() {
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  if (selecting) {
    drawSelectionScreen();
  } else {
    drawMainScreen();
  }
  screenDirty = false;
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
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  refreshDisplay();

  M5.Speaker.setVolume(0);
}

void loop() {
  static uint32_t awakeStart = millis();

  M5.update();

  bool enteredSelection = false;
  if (!selecting && dialClicked()) {
    selecting = true;
    picker = 0;
    screenDirty = true;
    enteredSelection = true;
  }

  if (selecting && !enteredSelection) {
    if (M5.BtnA.wasClicked()) {
      picker = (picker + CLEANER_COUNT - 1) % CLEANER_COUNT;
      screenDirty = true;
    }
    if (M5.BtnC.wasClicked()) {
      picker = (picker + 1) % CLEANER_COUNT;
      screenDirty = true;
    }
    if (M5.BtnB.wasClicked()) {
      saveCleaning(picker);
      selecting = false;
      awakeStart = millis();
      screenDirty = true;
    }
  }

  if (screenDirty) {
    refreshDisplay();
  }

  if (!selecting && (millis() - awakeStart) >= AWAKE_MS) {
    enterSleep();
  }

  delay(20);
}