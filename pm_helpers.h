#pragma once
// Helpers for the ESP-IDF power-management (automatic light sleep) setup in
// garden-weather-sensor-xiao.yaml.
//
// The cooling fan is driven by LEDC PWM. While the chip is in automatic light
// sleep the LEDC clock is gated, so the PWM output would stall or glitch
// between wake-ups. Rather than give up light sleep entirely, hold an
// ESP_PM_NO_LIGHT_SLEEP lock only while the fan is actually running (duty > 0).
// The fan is normally off (it only ramps above a 65C die temperature), so the
// lock is normally released and the chip is free to sleep.

#include "esp_pm.h"

namespace pm_helpers {

inline void set_fan_keeps_awake(bool fan_running) {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool held = false;
  if (lock == nullptr) {
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "fan_pwm", &lock) != ESP_OK) {
      lock = nullptr;
      return;
    }
  }
  if (fan_running && !held) {
    esp_pm_lock_acquire(lock);
    held = true;
  } else if (!fan_running && held) {
    esp_pm_lock_release(lock);
    held = false;
  }
}

}  // namespace pm_helpers
