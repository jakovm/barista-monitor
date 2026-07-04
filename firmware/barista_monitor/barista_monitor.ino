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

static constexpr uint8_t CLEAN_DAYS_WARN = 5;
static constexpr uint8_t CLEAN_DAYS_OVERDUE = 7;
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

  inverted = daysSinceClean >= CLEAN_DAYS_OVERDUE;
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

void drawSmiley(int cx, int cy, int mood) {
  uint16_t ink = inverted ? TFT_BLACK : TFT_WHITE;
  uint16_t paper = inverted ? TFT_WHITE : TFT_BLACK;

  int eyeY = cy - 10;
  int mouthY = cy + 12;

  if (mood >= 2) {
    M5.Display.fillCircle(cx - 14, eyeY, 4, ink);
    M5.Display.fillCircle(cx + 14, eyeY, 4, ink);
  } else {
    M5.Display.drawLine(cx - 18, eyeY - 2, cx - 10, eyeY + 4, ink);
    M5.Display.drawLine(cx - 10, eyeY + 4, cx - 18, eyeY + 4, ink);
    M5.Display.drawLine(cx + 18, eyeY - 2, cx + 10, eyeY + 4, ink);
    M5.Display.drawLine(cx + 10, eyeY + 4, cx + 18, eyeY + 4, ink);
  }

  if (mood >= 3) {
    M5.Display.drawCircle(cx, mouthY, 16, ink);
    M5.Display.drawCircle(cx, mouthY - 6, 16, paper);
  } else if (mood == 2) {
    M5.Display.drawCircle(cx, mouthY - 2, 12, ink);
    M5.Display.drawCircle(cx, mouthY - 8, 12, paper);
  } else if (mood == 1) {
    M5.Display.drawLine(cx - 14, mouthY, cx + 14, mouthY, ink);
  } else {
    M5.Display.drawCircle(cx, mouthY + 4, 12, ink);
    M5.Display.drawCircle(cx, mouthY - 8, 12, paper);
    M5.Display.drawLine(cx - 10, mouthY + 16, cx + 2, mouthY + 10, ink);
    M5.Display.drawLine(cx + 10, mouthY + 16, cx - 2, mouthY + 10, ink);
  }
}

int smileyMood() {
  if (daysSinceClean >= CLEAN_DAYS_OVERDUE) {
    return 0;
  }
  if (daysSinceClean >= CLEAN_DAYS_WARN) {
    return 1;
  }
  if (daysSinceClean >= 3) {
    return 2;
  }
  return 3;
}

void formatDate(uint32_t ymdKey, char* out, size_t outLen) {
  Ymd y = keyToYmd(ymdKey);
  snprintf(out, outLen, "%02d.%02d.%04d", y.day, y.month, y.year);
}

void drawBatteryIndicator() {
  if (!batteryLow) {
    return;
  }

  uint16_t ink = inverted ? TFT_BLACK : TFT_WHITE;
  int x = 150;
  int y = 182;
  M5.Display.drawRect(x, y, 18, 9, ink);
  M5.Display.fillRect(x + 18, y + 2, 3, 5, ink);
  M5.Display.fillRect(x + 2, y + 2, 10, 5, ink);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(ink, inverted ? TFT_WHITE : TFT_BLACK);
  M5.Display.setCursor(124, 176);
  M5.Display.print("<10T");
}

void drawMainScreen() {
  uint16_t paper = inverted ? TFT_WHITE : TFT_BLACK;
  uint16_t ink = inverted ? TFT_BLACK : TFT_WHITE;

  M5.Display.fillScreen(paper);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextColor(ink, paper);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_center);

  M5.Display.drawString("Siebtraeger", 100, 6);

  char dateBuf[16];
  formatDate(lastCleanYmd, dateBuf, sizeof(dateBuf));

  M5.Display.setTextDatum(top_left);
  M5.Display.setCursor(8, 28);
  M5.Display.print("Letzte Reinigung:");

  M5.Display.setCursor(8, 44);
  if (daysSinceClean == 0) {
    M5.Display.print("heute");
  } else {
    M5.Display.print(dateBuf);
  }

  M5.Display.setCursor(8, 62);
  M5.Display.printf("von %s", CLEANERS[lastCleaner]);

  M5.Display.setCursor(8, 82);
  if (daysSinceClean == 0) {
    M5.Display.print("frisch gereinigt");
  } else if (daysSinceClean == 1) {
    M5.Display.print("vor 1 Tag");
  } else {
    M5.Display.printf("vor %d Tagen", daysSinceClean);
  }

  if (daysSinceClean >= CLEAN_DAYS_OVERDUE) {
    M5.Display.setCursor(8, 102);
    M5.Display.print("JETZT reinigen!");
  } else if (daysSinceClean >= CLEAN_DAYS_WARN) {
    M5.Display.setCursor(8, 102);
    M5.Display.print("bald reinigen");
  }

  drawSmiley(100, 132, smileyMood());
  drawBatteryIndicator();

  M5.Display.setTextDatum(top_center);
  M5.Display.setCursor(100, 186);
  M5.Display.print("EXT: Reinigung");
}

void drawSelectionScreen() {
  static constexpr int ROW_X = 2;
  static constexpr int ROW_W = 196;
  static constexpr int ROW_H = 54;
  static constexpr int ROW_Y[] = {34, 92, 150};

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextFont(&fonts::AsciiFont8x16);
  M5.Display.setTextDatum(middle_center);

  for (uint8_t i = 0; i < CLEANER_COUNT; i++) {
    int y = ROW_Y[i];
    int cy = y + ROW_H / 2;

    if (i == picker) {
      M5.Display.fillRect(ROW_X, y, ROW_W, ROW_H, TFT_BLACK);
      M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Display.setTextSize(2);
    } else {
      M5.Display.drawRect(ROW_X, y, ROW_W, ROW_H, TFT_BLACK);
      M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
      M5.Display.setTextSize(1);
    }

    M5.Display.drawString(CLEANERS[i], 100, cy);
  }

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("Wer gereinigt?", 100, 4);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString("Drehen  |  EXT = OK", 100, 199);
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

  picker = lastCleaner;
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  refreshDisplay();

  M5.Speaker.setVolume(0);
}

void loop() {
  static uint32_t awakeStart = millis();

  M5.update();

  if (M5.BtnEXT.wasClicked()) {
    if (!selecting) {
      selecting = true;
      picker = lastCleaner;
    } else {
      saveCleaning(picker);
      selecting = false;
    }
    screenDirty = true;
  }

  if (selecting) {
    if (M5.BtnA.wasClicked() || M5.BtnC.wasClicked()) {
      if (M5.BtnA.wasClicked()) {
        picker = (picker + CLEANER_COUNT - 1) % CLEANER_COUNT;
      } else {
        picker = (picker + 1) % CLEANER_COUNT;
      }
      screenDirty = true;
    }
    if (M5.BtnB.wasClicked()) {
      saveCleaning(picker);
      selecting = false;
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