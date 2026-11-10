// Firmware probation after a web upload: decide whether to keep the new firmware or roll back
#include "ota_confirm.h"

ota_confirm_action_t ota_confirm_decide(const ota_confirm_t *c, ota_confirm_reason_t *reason)
{
    *reason = OTA_REASON_NONE;
    if (c->keep || c->web_restart)
        return OTA_CONFIRM_KEEP;
    if (c->rollback)
        *reason = OTA_REASON_MANUAL;
    else if (!c->got_ip && c->uptime >= OTA_CONFIRM_IP_WAIT)
        *reason = OTA_REASON_NO_IP;
    else if (c->uptime >= OTA_CONFIRM_WINDOW)
        *reason = OTA_REASON_TIMEOUT;
    else
        return OTA_CONFIRM_WAIT;
    if (!c->can_rollback)
        return OTA_CONFIRM_KEEP; // Nothing to go back to, so keep rather than reboot in a loop
    return OTA_CONFIRM_ROLLBACK;
}

const char *ota_confirm_reason_text(ota_confirm_reason_t reason)
{
    switch (reason) {
    case OTA_REASON_MANUAL:
        return "manual";
    case OTA_REASON_NO_IP:
        return "no-ip";
    case OTA_REASON_TIMEOUT:
        return "timeout";
    default:
        return "";
    }
}
