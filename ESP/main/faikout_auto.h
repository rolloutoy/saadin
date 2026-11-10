// Faikout auto: decisions taken by the local auto control in the main loop
// Pure logic, no ESP-IDF calls, so it also builds as a host unit test (see test/faikout_auto_test.c)
#pragma once

#include <stdint.h>

// What the review of the last two sample periods (tsample) asks for
typedef enum {
    AUTO_REVIEW_NONE,
    AUTO_REVIEW_MODE,     // Swap heat/cool, and drop the fan to 1 if fan_down
    AUTO_REVIEW_FAN_DOWN, // Reduce the fan
    AUTO_REVIEW_FAN_UP,   // Increase the fan
    AUTO_REVIEW_OFF,      // Power off
    AUTO_REVIEW_ON,       // Power on
    AUTO_REVIEW_ON_MODE,  // Power on and swap heat/cool
} auto_review_action_t;

typedef struct {
    int approaching, beyond, total;    // Sample counts in the period just ended
    int approaching2, beyond2, total2; // Sample counts in that period and the one before
    uint8_t power;                     // Unit is on
    uint8_t slave;                     // Another unit is controlling the mode
    uint8_t lock_mode;                 // Do not swap heat/cool (lockmode)
    uint8_t autopower;                 // Auto power on/off is enabled
    uint8_t fan_down;                  // Fan can be reduced
    uint8_t fan_up;                    // Fan can be increased
    uint8_t out_of_band;               // Temperature is autoptemp or more outside min/max
} auto_review_t;

// Action at the end of a sample period
auto_review_action_t auto_review(const auto_review_t *r);

// daikin.env after a check of the BLE sensor. reading is its temperature, or NAN if it has none. *from_ble says
// whether env holds a BLE reading.
float auto_ble_env(float env, uint8_t *from_ble, float reading);
