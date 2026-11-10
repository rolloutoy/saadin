// Firmware probation after a web upload: decide whether to keep the new firmware or roll back
// Pure logic, no ESP-IDF calls, so it also builds as a host unit test (see test/ota_confirm_test.c)
#pragma once

#include <stdint.h>

#define OTA_CONFIRM_WINDOW 600  // Seconds from boot for a person to click Keep
#define OTA_CONFIRM_IP_WAIT 180 // Seconds from boot to get an IP address

typedef enum {
    OTA_CONFIRM_WAIT,     // Still on probation, nothing to do yet
    OTA_CONFIRM_KEEP,     // Mark the running firmware valid
    OTA_CONFIRM_ROLLBACK, // Mark it invalid and boot the previous firmware
} ota_confirm_action_t;

typedef enum {
    OTA_REASON_NONE,
    OTA_REASON_MANUAL,  // Roll back now clicked
    OTA_REASON_NO_IP,   // No IP address within OTA_CONFIRM_IP_WAIT
    OTA_REASON_TIMEOUT, // Nobody clicked Keep within OTA_CONFIRM_WINDOW
} ota_confirm_reason_t;

typedef struct {
    uint32_t uptime;          // Seconds since boot
    uint8_t got_ip : 1;       // Has had an IP address since boot
    uint8_t keep : 1;         // Keep clicked in the web UI
    uint8_t rollback : 1;     // Roll back now clicked in the web UI
    uint8_t web_restart : 1;  // Restart requested from the web UI
    uint8_t can_rollback : 1; // The other slot holds firmware to go back to
} ota_confirm_t;

// What to do now with firmware on probation. If the result is to keep it only because there is nothing to roll back
// to, *reason says why it would have rolled back, so the caller can log it.
ota_confirm_action_t ota_confirm_decide(const ota_confirm_t *c, ota_confirm_reason_t *reason);

// Short reason text for the rollback notice and MQTT: "manual", "no-ip" or "timeout"
const char *ota_confirm_reason_text(ota_confirm_reason_t reason);
