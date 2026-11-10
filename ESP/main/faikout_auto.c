// Faikout auto: decisions taken by the local auto control in the main loop
#include <math.h>
#include "faikout_auto.h"

auto_review_action_t auto_review(const auto_review_t *r)
{
    if (r->power) { // Daikin is on
        // A lot more beyond than total counts and no approaching in the last two cycles
        // Time to switch modes (heating/cooling) and reduce fan to minimum
        if ((r->beyond2 * 2 > r->total2 || r->slave) && !r->approaching2) { // Mode switch
            if (!r->lock_mode)
                return AUTO_REVIEW_MODE;
            // Mode is locked, so turn off rather than keep overshooting (not just for being a slave)
            if (r->autopower && r->beyond2 * 2 > r->total2)
                return AUTO_REVIEW_OFF;
            return AUTO_REVIEW_NONE;
        }
        // Less approaching, but still close to min in heating or max in cooling
        // Time to reduce the fan a bit
        if (r->fan_down && r->approaching2 * 10 < r->total2 * 7)
            return AUTO_REVIEW_FAN_DOWN;
        // A lot of approaching means still far away from desired temp
        // Time to increase the fan speed
        if (r->fan_up && !r->slave && r->approaching2 * 10 > r->total2 * 9)
            return AUTO_REVIEW_FAN_UP;
        // No Approaching and no Beyond, so it's in desired range (autot +/- autor)
        // Turn off as 100% in band for last two period
        if (r->autopower && !r->approaching2 && !r->beyond2)
            return AUTO_REVIEW_OFF;
        return AUTO_REVIEW_NONE;
    }
    // Daikin is off
    // Turn on as 100% out of band for the last period
    // With the mode locked, only when approaching, as beyond would need the other mode and it would turn off again
    if (r->autopower && (r->approaching == r->total || r->beyond == r->total) && r->out_of_band &&
        (!r->lock_mode || r->beyond != r->total))
        return r->beyond2 == r->total2 ? AUTO_REVIEW_ON_MODE : AUTO_REVIEW_ON;
    return AUTO_REVIEW_NONE;
}

float auto_ble_env(float env, uint8_t *from_ble, float reading)
{
    if (!isnan(reading)) {
        *from_ble = 1;
        return reading;
    }
    return env;
}
