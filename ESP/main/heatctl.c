// Säädin heating controller (heatctl), see docs/heat-control.md
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heatctl.h"

static const char *skipspace(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

static const char *parse_table(heatctl_table_t *t, const char *s)
{
    t->n = 0;
    s = skipspace(s);
    while (*s) {
        if (t->n >= HEATCTL_RUNGS_MAX)
            return "Too many rungs";
        char *e;
        long d = strtol(s, &e, 10);
        if (e == s)
            return "Bad demand";
        if (d < 30 || d > 100)
            return "Demand not 30 to 100";
        s = e;
        uint8_t q = 0;
        if (*s == 'q') {
            if (d != 100)
                return "Only 100 takes q";
            q = 1;
            s++;
        }
        s = skipspace(s);
        if (*s++ != ':')
            return "Expected demand:watts";
        float w = strtof(s, &e);
        if (e == s)
            return "Bad watts";
        if (!(w > 0))
            return "Watts not positive";
        s = skipspace(e);
        heatctl_rung_t *r = &t->rung[t->n];
        if (t->n && (w <= r[-1].watts || d < r[-1].demand))
            return "Rungs not increasing";
        r->demand = d;
        r->watts = w;
        r->quiet = (d < 100 || q); // QO on for every rung except plain 100
        t->n++;
        if (*s == ',')
            s = skipspace(s + 1);
        else if (*s)
            return "Expected ,";
        else
            break;
        if (!*s)
            return "Trailing ,";
    }
    return NULL;
}

const char *heatctl_parse_table(heatctl_table_t *t, const char *s)
{
    const char *err = NULL;
    if (s && *skipspace(s))
        err = parse_table(t, s);
    if (!s || !*skipspace(s) || err)
        parse_table(t, HEATCTL_TABLE_DEFAULT);
    return err;
}

const char *heatctl_parse_curve(heatctl_curve_t *c, const char *s)
{
    c->n = 0;
    if (!s)
        return NULL;
    s = skipspace(s);
    const char *err = NULL;
    while (*s && !err) {
        if (c->n >= HEATCTL_CURVE_MAX) {
            err = "Too many points";
            break;
        }
        char *e;
        float o = strtof(s, &e);
        if (e == s) {
            err = "Bad outdoor temperature";
            break;
        }
        s = skipspace(e);
        if (*s++ != ':') {
            err = "Expected outdoor:offset";
            break;
        }
        float v = strtof(s, &e);
        if (e == s) {
            err = "Bad offset";
            break;
        }
        s = skipspace(e);
        if (c->n && o <= c->outdoor[c->n - 1]) {
            err = "Outdoor temperatures not increasing";
            break;
        }
        c->outdoor[c->n] = o;
        c->offset[c->n] = v;
        c->n++;
        if (*s == ',') {
            s = skipspace(s + 1);
            if (!*s)
                err = "Trailing ,";
        } else if (*s)
            err = "Expected ,";
    }
    if (err)
        c->n = 0;
    return err;
}

float heatctl_curve_offset(const heatctl_curve_t *c, float outdoor)
{
    if (!c->n || isnan(outdoor))
        return 0;
    if (outdoor <= c->outdoor[0])
        return c->offset[0];
    for (int i = 1; i < c->n; i++)
        if (outdoor < c->outdoor[i])
            return c->offset[i - 1] + (c->offset[i] - c->offset[i - 1]) * (outdoor - c->outdoor[i - 1]) /
                                          (c->outdoor[i] - c->outdoor[i - 1]);
    return c->offset[c->n - 1];
}

const char *heatctl_state_name(heatctl_state_t state)
{
    static const char *const names[] = {"off", "idle", "demand", "cold", "nosensor"};
    return state < sizeof(names) / sizeof(*names) ? names[state] : "?";
}

const char *heatctl_sub_name(heatctl_sub_t sub)
{
    static const char *const names[] = {"", "defrost", "hold", "rest"};
    return sub < sizeof(names) / sizeof(*names) ? names[sub] : "?";
}

float heatctl_ble_env(float env, uint8_t *from_ble, float reading)
{
    if (!isnan(reading)) {
        *from_ble = 1;
        return reading;
    }
    if (*from_ble) { // Sensor has gone stale or missing, so its last reading is no longer the room temperature
        *from_ble = 0;
        return NAN;
    }
    return env; // Not from the BLE sensor (e.g. MQTT), so leave it
}

// Rung for a demand (%): the highest rung at or below it, plain 100 before 100q
static int rung_for(const heatctl_table_t *t, int demand)
{
    int r = 0;
    for (int i = 0; i < t->n; i++)
        if (t->rung[i].demand <= demand)
            r = i;
    return r;
}

// Highest rung allowed, in the post-defrost hold or not
static int rung_top(const heatctl_params_t *p, uint8_t hold)
{
    int top = p->table.n - 1;
    if (!p->demand_full) // No plain 100
        while (top > 0 && !p->table.rung[top].quiet)
            top--;
    if (hold) // Capped at 95, no 100q or plain 100
        while (top > 0 && p->table.rung[top].demand > 95)
            top--;
    return top;
}

// Highest rung whose watts are at or below w, up to top
static int rung_wanted(const heatctl_table_t *t, int top, float w)
{
    int r = 0;
    for (int i = 1; i <= top; i++)
        if (t->rung[i].watts <= w)
            r = i;
    return r;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float round_half(float v)
{
    return roundf(v * 2) / 2;
}

static void reason(heatctl_out_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reason(heatctl_out_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->reason, sizeof(out->reason), fmt, ap);
    va_end(ap);
}

static void enter(heatctl_t *s, heatctl_out_t *out, uint32_t now, heatctl_state_t state, heatctl_sub_t sub)
{
    s->state = state;
    s->since = now;
    s->sub = sub;
    s->sub_since = now;
    out->changed = 1;
}

static void enter_sub(heatctl_t *s, heatctl_out_t *out, uint32_t now, heatctl_sub_t sub)
{ // Same state, so keep its time
    s->sub = sub;
    s->sub_since = now;
    out->changed = 1;
}

static heatctl_sub_t keep_hold(const heatctl_t *s)
{ // The post-defrost hold carries over between DEMAND and COLD
    return s->sub == HEATCTL_SUB_HOLD ? HEATCTL_SUB_HOLD : HEATCTL_SUB_NONE;
}

static void enter_demand(heatctl_t *s, const heatctl_params_t *p, heatctl_out_t *out, uint32_t now, int demand)
{ // DEMAND with the integral at the watts of this demand's rung
    enter(s, out, now, HEATCTL_DEMAND, keep_hold(s));
    s->integral = p->table.rung[rung_for(&p->table, demand)].watts;
    s->rung = -1; // Set at once
}

static void enter_cold(heatctl_t *s, const heatctl_params_t *p, heatctl_out_t *out, uint32_t now)
{
    enter(s, out, now, HEATCTL_COLD, keep_hold(s));
    s->cold_integral = clampf(p->cold_set, p->tmin, p->tmax);
    s->setpoint = round_half(s->cold_integral);
    s->setpoint_at = now;
}

// Track how long a condition has held
static void held(uint8_t cond, uint8_t *is, uint32_t *since, uint32_t now)
{
    if (cond && !*is)
        *since = now;
    *is = cond;
}

heatctl_out_t heatctl_step(heatctl_t *s, const heatctl_params_t *p, const heatctl_in_t *in, uint32_t now)
{
    heatctl_out_t out = {0};
    uint32_t dt = (s->ticked && now > s->last) ? now - s->last : 0;
    s->ticked = 1;
    s->last = now;

    // Room, smoothed, restarted after a gap
    s->room_fresh = !isnan(in->room);
    if (s->room_fresh) {
        if (!s->room_ok || now - s->room_at > p->smooth)
            s->room = in->room;
        else
            s->room += (in->room - s->room) * dt / (p->smooth + dt);
        s->room_ok = 1;
        s->room_at = now;
    }

    // Defrost sub-state, from liquid (DEMAND and COLD only)
    int defrost_rest = 0;
    if (s->state == HEATCTL_DEMAND || s->state == HEATCTL_COLD) {
        if (!isnan(in->liquid) && in->liquid >= p->defrost_exit)
            s->armed = 1; // Heating has been established, so a drop in liquid now is a defrost
        if (s->sub == HEATCTL_SUB_DEFROST) {
            uint8_t cap = (now - s->sub_since >= p->defrost_max);
            if (cap || (!isnan(in->liquid) && in->liquid >= p->defrost_exit)) {
                int n = 0;
                for (int i = 0; i < s->defrosts; i++)
                    if (now - s->defrost_at[i] < p->defrost_window)
                        n++;
                if (cap)
                    reason(&out, "defrost cap %lu min, liquid %.1f", (unsigned long)p->defrost_max / 60, in->liquid);
                else
                    reason(&out, "defrost over, liquid %.1f >= %.1f", in->liquid, p->defrost_exit);
                out.resend_fan = 1;
                if (n > p->defrost_loops)
                    defrost_rest = n;
                else
                    enter_sub(s, &out, now, HEATCTL_SUB_HOLD);
            }
        } else if (s->armed && !isnan(in->liquid) && in->liquid < p->defrost_enter) {
            s->armed = 0;
            enter_sub(s, &out, now, HEATCTL_SUB_DEFROST);
            if (s->defrosts == HEATCTL_DEFROSTS_MAX) {
                memmove(s->defrost_at, s->defrost_at + 1, sizeof(*s->defrost_at) * (HEATCTL_DEFROSTS_MAX - 1));
                s->defrosts--;
            }
            s->defrost_at[s->defrosts++] = now;
            reason(&out, "defrost, liquid %.1f < %.1f", in->liquid, p->defrost_enter);
        } else if (s->sub == HEATCTL_SUB_HOLD && now - s->sub_since >= p->defrost_hold) {
            enter_sub(s, &out, now, HEATCTL_SUB_NONE);
            reason(&out, "post-defrost hold over");
        }
    }

    // Outdoor, smoothed, frozen during defrost (the unit's reading jumps)
    if (!isnan(in->outdoor) && s->sub != HEATCTL_SUB_DEFROST) {
        if (!s->outdoor_ok)
            s->outdoor = in->outdoor;
        else
            s->outdoor += (in->outdoor - s->outdoor) * dt / (HEATCTL_OUTDOOR_SMOOTH + dt);
        s->outdoor_ok = 1;
    }
    float outdoor = s->outdoor_ok ? s->outdoor : NAN;
    held(outdoor < p->cold_enter, &s->is_cold, &s->cold_since, now); // False if unknown
    held(outdoor > p->cold_exit, &s->is_warm, &s->warm_since, now);

    float curve = heatctl_curve_offset(&p->curve, outdoor);
    float target = p->target + curve;
    out.target = target;
    out.curve = curve;
    out.room = s->room_ok ? s->room : NAN;
    out.outdoor = outdoor;
    out.heatw = NAN;
    out.fan = HEATCTL_FAN_LEAVE;

    if (!in->enabled || !in->power || !in->heat) {
        if (s->state != HEATCTL_OFF) {
            enter(s, &out, now, HEATCTL_OFF, HEATCTL_SUB_NONE);
            reason(&out, "%s", !in->enabled ? "disabled" : !in->power ? "unit off" : "unit not heating");
        }
        out.state = HEATCTL_OFF;
        out.integral = s->integral;
        return out;
    }

    if (s->state == HEATCTL_OFF) { // Fresh start
        s->sub = HEATCTL_SUB_NONE;
        s->armed = (!isnan(in->liquid) && in->liquid >= p->defrost_exit);
        s->defrosts = 0;
        if (!s->room_fresh)
            s->room_at = now; // Stale counts from the start
        enter_demand(s, p, &out, now, p->demand_start);
        reason(&out, "start");
    }

    if (defrost_rest) { // Defrost-loop watchdog
        s->resume = s->state;
        enter(s, &out, now, HEATCTL_IDLE, HEATCTL_SUB_REST);
        s->defrosts = 0;
        reason(&out, "%d defrosts in %lu min, rest %lu min", defrost_rest, (unsigned long)p->defrost_window / 60,
               (unsigned long)p->defrost_rest / 60);
    }

    uint8_t stale = !s->room_fresh;
    float room = s->room;
    if (s->state != HEATCTL_NOSENSOR && stale && now - s->room_at >= p->stale) {
        enter(s, &out, now, HEATCTL_NOSENSOR, HEATCTL_SUB_NONE);
        reason(&out, "no room reading for %lu min", (unsigned long)(now - s->room_at) / 60);
    } else if (s->state == HEATCTL_NOSENSOR && !stale) {
        enter_demand(s, p, &out, now, p->demand_start);
        reason(&out, "room reading back, %.2f", room);
    } else if (!stale && s->sub != HEATCTL_SUB_DEFROST) { // No transitions on a stale reading or during defrost
        switch (s->state) {
        case HEATCTL_DEMAND:
            if (room > target + p->idle_over && now - s->since >= p->demand_minrun) {
                enter(s, &out, now, HEATCTL_IDLE, HEATCTL_SUB_NONE);
                reason(&out, "room %.2f > target %.2f+%.1f", room, target, p->idle_over);
            } else if (s->is_cold && now - s->cold_since >= p->cold_hold) {
                enter_cold(s, p, &out, now);
                reason(&out, "outdoor %.1f < %.1f for %lu min", outdoor, p->cold_enter,
                       (unsigned long)p->cold_hold / 60);
            }
            break;
        case HEATCTL_IDLE:
            if (s->sub == HEATCTL_SUB_REST) {
                if (now - s->since >= p->defrost_rest) {
                    if (s->resume == HEATCTL_COLD)
                        enter_cold(s, p, &out, now);
                    else
                        enter_demand(s, p, &out, now, p->table.rung[0].demand);
                    reason(&out, "defrost rest over");
                }
            } else if (room < target - p->idle_under && now - s->since >= p->idle_min) {
                enter_demand(s, p, &out, now, p->table.rung[0].demand);
                reason(&out, "room %.2f < target %.2f-%.1f", room, target, p->idle_under);
            }
            break;
        case HEATCTL_COLD:
            if (s->is_warm && now - s->warm_since >= p->cold_hold) {
                enter_demand(s, p, &out, now, 95);
                reason(&out, "outdoor %.1f > %.1f for %lu min", outdoor, p->cold_exit,
                       (unsigned long)p->cold_hold / 60);
            }
            break;
        default:
            break;
        }
    }

    // Outputs
    float e = target - room;
    uint8_t freeze = stale || s->sub == HEATCTL_SUB_DEFROST || out.changed; // On a change the integral starts as set
    uint8_t noraise = (s->sub == HEATCTL_SUB_HOLD);
    switch (s->state) {
    case HEATCTL_DEMAND: {
        const heatctl_table_t *t = &p->table;
        int top = rung_top(p, noraise);
        int clamp = rung_top(p, 0); // The hold stops growth, it does not pull the integral down
        int was = s->rung;
        if (!freeze) { // PI, the integral in W, with anti-windup at the top and bottom rungs
            float di = p->kp * e * dt / (p->ti > 0 ? p->ti : 1);
            int r = rung_wanted(t, top, p->kp * e + s->integral);
            if (di > 0 && (r >= top || noraise))
                di = 0;
            if (di < 0 && r <= 0)
                di = 0;
            s->integral = clampf(s->integral + di, t->rung[0].watts, t->rung[clamp].watts);
        }
        if (!stale)
            s->heatw = p->kp * e + s->integral;
        else if (s->rung < 0)
            s->heatw = s->integral; // Starting with no reading
        int want = rung_wanted(t, top, s->heatw);
        if (s->rung < 0) // Just entered
            s->rung = want;
        else if (s->sub == HEATCTL_SUB_DEFROST)
            ; // Hold
        else if (s->rung > top)
            s->rung = top; // Cap at once
        else if (stale)
            ; // Hold
        else if (want != s->rung && now - s->rung_at >= p->demand_rate && !(noraise && want > s->rung))
            s->rung = want;
        if (s->rung != was)
            s->rung_at = now;
        out.demand = t->rung[s->rung].demand;
        out.quiet = t->rung[s->rung].quiet || s->sub == HEATCTL_SUB_DEFROST;
        out.setpoint = p->demand_set;
        out.fan = p->demand_fan;
        out.heatw = s->heatw;
        out.integral = s->integral;
        break;
    }
    case HEATCTL_IDLE:
        out.demand = p->table.rung[0].demand;
        out.quiet = 1;
        out.setpoint = p->idle_temp;
        out.fan = p->idle_fan;
        out.integral = s->integral;
        break;
    case HEATCTL_COLD:
        if (!freeze) { // PI on the setpoint, the integral in °C
            float di = p->cold_kp * e * dt / (p->cold_ti > 0 ? p->cold_ti : 1);
            if (di > 0 && noraise)
                di = 0;
            s->cold_integral = clampf(s->cold_integral + di, p->tmin, p->tmax);
        }
        if (!stale && s->sub != HEATCTL_SUB_DEFROST && now - s->setpoint_at >= p->cold_rate) {
            float want = round_half(clampf(p->cold_kp * e + s->cold_integral, p->tmin, p->tmax));
            if (noraise && want > s->setpoint)
                want = s->setpoint;
            if (want != s->setpoint) { // Steps of 0.5 at most
                s->setpoint += clampf(want - s->setpoint, -0.5, 0.5);
                s->setpoint_at = now;
            }
        }
        out.demand = 100;
        out.quiet = 0;
        out.setpoint = s->setpoint;
        out.fan = p->cold_fan;
        out.integral = s->cold_integral;
        for (int i = p->table.n - 1; i >= 0; i--)
            if (!p->table.rung[i].quiet) {
                out.heatw = p->table.rung[i].watts;
                break;
            }
        break;
    case HEATCTL_NOSENSOR: // Plain unit thermostat on the unit's own sensor
        out.demand = 100;
        out.quiet = 0;
        out.setpoint = target;
        out.integral = s->integral;
        break;
    default:
        break;
    }
    out.setpoint = clampf(out.setpoint, p->tmin, p->tmax);
    out.state = s->state;
    out.sub = s->sub;
    return out;
}
