// Host unit test for the Faikout auto decisions (faikout_auto.c)
// Build and run with: make -C ESP test

#include <math.h>
#include <stdio.h>
#include "../faikout_auto.h"

static int failed = 0;

static void check(const char *name, auto_review_t r, auto_review_action_t want)
{
    auto_review_action_t got = auto_review(&r);
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", name, got, want);
        failed++;
    } else
        printf("ok   %s\n", name);
}

static void check_env(const char *name, float got, float want)
{
    if (isnan(got) != isnan(want) || (!isnan(want) && got != want)) {
        printf("FAIL %s: got %.2f, want %.2f\n", name, got, want);
        failed++;
    } else
        printf("ok   %s\n", name);
}

// Sample counts over two periods of 10 samples each
#define IN_BAND .total = 10, .total2 = 20
#define APPROACHING .approaching = 10, .approaching2 = 20, .total = 10, .total2 = 20
#define BEYOND .beyond = 10, .beyond2 = 20, .total = 10, .total2 = 20

int main(void)
{
    // Unit on
    check("in band, auto power off", (auto_review_t){IN_BAND, .power = 1, .autopower = 1}, AUTO_REVIEW_OFF);
    check("in band, no auto power", (auto_review_t){IN_BAND, .power = 1}, AUTO_REVIEW_NONE);
    check("approaching, fan up", (auto_review_t){APPROACHING, .power = 1, .fan_up = 1}, AUTO_REVIEW_FAN_UP);
    check("approaching, slave, no fan up", (auto_review_t){APPROACHING, .power = 1, .fan_up = 1, .slave = 1},
          AUTO_REVIEW_NONE);
    check("mostly in band, fan down",
          (auto_review_t){.approaching2 = 2, .total = 10, .total2 = 20, .power = 1, .fan_down = 1},
          AUTO_REVIEW_FAN_DOWN);
    check("overshoot, swap mode", (auto_review_t){BEYOND, .power = 1, .autopower = 1}, AUTO_REVIEW_MODE);
    check("mostly overshoot, swap mode",
          (auto_review_t){.beyond = 6, .beyond2 = 11, .total = 10, .total2 = 20, .power = 1, .autopower = 1},
          AUTO_REVIEW_MODE);
    check("slave, swap mode", (auto_review_t){IN_BAND, .power = 1, .slave = 1}, AUTO_REVIEW_MODE);

    // Unit on, mode locked
    check("overshoot, lockmode, no auto power", (auto_review_t){BEYOND, .power = 1, .lock_mode = 1}, AUTO_REVIEW_NONE);
    check("slave in band, lockmode, stays on",
          (auto_review_t){IN_BAND, .power = 1, .slave = 1, .lock_mode = 1, .autopower = 1}, AUTO_REVIEW_NONE);

    // Unit off
    check("off, approaching, out of band, on", (auto_review_t){APPROACHING, .autopower = 1, .out_of_band = 1},
          AUTO_REVIEW_ON);
    check("off, approaching, within autoptemp", (auto_review_t){APPROACHING, .autopower = 1}, AUTO_REVIEW_NONE);
    check("off, approaching, no auto power", (auto_review_t){APPROACHING, .out_of_band = 1}, AUTO_REVIEW_NONE);
    check("off, overshoot, on in other mode", (auto_review_t){BEYOND, .autopower = 1, .out_of_band = 1},
          AUTO_REVIEW_ON_MODE);
    check("off, approaching, lockmode, on",
          (auto_review_t){APPROACHING, .lock_mode = 1, .autopower = 1, .out_of_band = 1}, AUTO_REVIEW_ON);
    check("off, overshoot, lockmode, stays off",
          (auto_review_t){BEYOND, .lock_mode = 1, .autopower = 1, .out_of_band = 1}, AUTO_REVIEW_NONE);

    // BLE sensor temperature
    uint8_t from_ble = 0;
    check_env("BLE reading used", auto_ble_env(NAN, &from_ble, 21.5), 21.5);
    check_env("BLE reading updated", auto_ble_env(21.5, &from_ble, 21.25), 21.25);
    check_env("BLE still gone, MQTT env kept", auto_ble_env(19.0, &from_ble, NAN), 19.0);
    check_env("BLE back, its reading used", auto_ble_env(19.0, &from_ble, 20.0), 20.0);
    from_ble = 0;
    check_env("no BLE reading yet, MQTT env kept", auto_ble_env(18.5, &from_ble, NAN), 18.5);

    if (failed)
        printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
