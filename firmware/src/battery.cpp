#include "battery.h"

#include "config.h"
#include "pins.h"

namespace battery {

static float vFilt = 0;
static uint32_t nextRead = 0;

// Li-ion open-circuit voltage -> state of charge
static const float kV[] = {3.30f, 3.50f, 3.60f, 3.68f, 3.74f, 3.78f, 3.83f, 3.90f, 3.98f, 4.07f, 4.18f};
static const uint8_t kP[] = {0, 5, 10, 20, 30, 40, 50, 60, 70, 85, 100};

static float sample() {
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  analogSampleTime(40);  // high source impedance (5 MOhm), C1 holds the charge
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogRead(PIN_BATT_ADC);
  analogReference(AR_DEFAULT);
  float v = (acc / 8.0f) * 3.0f / 4096.0f * BATT_DIVIDER;
  return v * (cfg.battCalX1000 / 1000.0f);
}

void begin() {
  vFilt = sample();
  nextRead = millis() + 2000;
}

void loop() {
  if ((int32_t)(millis() - nextRead) < 0) return;
  nextRead = millis() + 2000;
  float v = sample();
  vFilt = (fabsf(v - vFilt) > 0.3f) ? v : vFilt * 0.8f + v * 0.2f;
}

float volts() { return vFilt; }

uint8_t percent() {
  float v = vFilt;
  if (v <= kV[0]) return 0;
  const int n = sizeof(kV) / sizeof(kV[0]);
  if (v >= kV[n - 1]) return 100;
  for (int i = 1; i < n; i++) {
    if (v < kV[i]) {
      float f = (v - kV[i - 1]) / (kV[i] - kV[i - 1]);
      return (uint8_t)(kP[i - 1] + f * (kP[i] - kP[i - 1]) + 0.5f);
    }
  }
  return 100;
}

bool usbPowered() { return NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk; }

}  // namespace battery
