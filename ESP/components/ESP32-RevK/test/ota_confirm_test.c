// Host unit test for the firmware probation decision (ota_confirm.c)
// Build and run with: make -C ESP/components/ESP32-RevK test

#include <stdio.h>
#include <string.h>
#include "../ota_confirm.h"

static int failed = 0;

static void check(const char *name, ota_confirm_t c, ota_confirm_action_t want, ota_confirm_reason_t want_reason)
{
    ota_confirm_reason_t reason = OTA_REASON_NONE;
    ota_confirm_action_t got = ota_confirm_decide(&c, &reason);
    if (got != want || reason != want_reason) {
        printf("FAIL %s: got action %d reason %d, want action %d reason %d\n", name, got, reason, want, want_reason);
        failed++;
    } else
        printf("ok   %s\n", name);
}

int main(void)
{
    // Waiting for a person
    check("just booted", (ota_confirm_t){.uptime = 0, .can_rollback = 1}, OTA_CONFIRM_WAIT, OTA_REASON_NONE);
    check("online, window open", (ota_confirm_t){.uptime = OTA_CONFIRM_WINDOW - 1, .got_ip = 1, .can_rollback = 1},
          OTA_CONFIRM_WAIT, OTA_REASON_NONE);
    check("no IP yet, still in time", (ota_confirm_t){.uptime = OTA_CONFIRM_IP_WAIT - 1, .can_rollback = 1},
          OTA_CONFIRM_WAIT, OTA_REASON_NONE);

    // Kept
    check("Keep clicked", (ota_confirm_t){.uptime = 5, .got_ip = 1, .keep = 1, .can_rollback = 1}, OTA_CONFIRM_KEEP,
          OTA_REASON_NONE);
    check("Keep needs no minimum uptime", (ota_confirm_t){.uptime = 0, .keep = 1, .can_rollback = 1}, OTA_CONFIRM_KEEP,
          OTA_REASON_NONE);
    check("restart from web UI keeps", (ota_confirm_t){.uptime = 30, .got_ip = 1, .web_restart = 1, .can_rollback = 1},
          OTA_CONFIRM_KEEP, OTA_REASON_NONE);
    check("Keep wins over a window that just ran out",
          (ota_confirm_t){.uptime = OTA_CONFIRM_WINDOW, .got_ip = 1, .keep = 1, .can_rollback = 1}, OTA_CONFIRM_KEEP,
          OTA_REASON_NONE);

    // Rolled back
    check("Roll back now clicked", (ota_confirm_t){.uptime = 5, .got_ip = 1, .rollback = 1, .can_rollback = 1},
          OTA_CONFIRM_ROLLBACK, OTA_REASON_MANUAL);
    check("no IP after 3 minutes", (ota_confirm_t){.uptime = OTA_CONFIRM_IP_WAIT, .can_rollback = 1},
          OTA_CONFIRM_ROLLBACK, OTA_REASON_NO_IP);
    check("window ran out", (ota_confirm_t){.uptime = OTA_CONFIRM_WINDOW, .got_ip = 1, .can_rollback = 1},
          OTA_CONFIRM_ROLLBACK, OTA_REASON_TIMEOUT);
    check("IP lost after it was there does not count as no IP",
          (ota_confirm_t){.uptime = OTA_CONFIRM_IP_WAIT + 10, .got_ip = 1, .can_rollback = 1}, OTA_CONFIRM_WAIT,
          OTA_REASON_NONE);

    // Nothing to go back to: keep rather than reboot in a loop, and say why
    check("timeout, nothing to roll back to", (ota_confirm_t){.uptime = OTA_CONFIRM_WINDOW, .got_ip = 1},
          OTA_CONFIRM_KEEP, OTA_REASON_TIMEOUT);
    check("Roll back now, nothing to roll back to", (ota_confirm_t){.uptime = 5, .got_ip = 1, .rollback = 1},
          OTA_CONFIRM_KEEP, OTA_REASON_MANUAL);

    // Reason text used in the notice and MQTT
    if (strcmp(ota_confirm_reason_text(OTA_REASON_MANUAL), "manual") ||
        strcmp(ota_confirm_reason_text(OTA_REASON_NO_IP), "no-ip") ||
        strcmp(ota_confirm_reason_text(OTA_REASON_TIMEOUT), "timeout")) {
        printf("FAIL reason text\n");
        failed++;
    } else
        printf("ok   reason text\n");

    if (failed)
        printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
