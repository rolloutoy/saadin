// Saadin heating controller (heatctl), see docs/heat-control.md
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
        if (t->n >= HEATCTL_LEVELS_MAX)
            return "Too many levels";
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
        heatctl_level_t *r = &t->level[t->n];
        if (t->n && (w <= r[-1].watts || d < r[-1].demand))
            return "Levels not increasing";
        r->demand = d;
        r->watts = w;
        r->quiet = (d < 100 || q); // QO on for every level except plain 100
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

int heatctl_format_table(const heatctl_table_t *t, char *buf, size_t len)
{
    int n = 0;
    if (len)
        *buf = 0;
    for (int i = 0; i < t->n; i++) {
        const heatctl_level_t *r = &t->level[i];
        size_t left = (size_t)n < len ? len - n : 0; // Nothing more is written once buf is full
        n += snprintf(left ? buf + n : NULL, left, "%s%u%s:%g", i ? "," : "", r->demand,
                      r->demand == 100 && r->quiet ? "q" : "", r->watts);
    }
    return n;
}

const char *heatctl_parse_table(heatctl_table_t *t, const char *s, const char *def)
{
    const char *err = NULL;
    if (s && *skipspace(s))
        err = parse_table(t, s);
    if (!s || !*skipspace(s) || err)
        parse_table(t, def);
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

const char *heatctl_parse_fanmap(heatctl_fanmap_t *m, const char *s)
{
    m->n = 0;
    if (!s)
        return NULL;
    s = skipspace(s);
    const char *err = NULL;
    while (*s && !err) {
        if (m->n >= HEATCTL_CURVE_MAX) {
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
            err = "Expected outdoor:fan";
            break;
        }
        s = skipspace(s);
        int c = toupper((unsigned char)*s);
        int8_t f = c == 'A' ? 0 : c == 'Q' ? 6 : c >= '1' && c <= '5' ? c - '0' : -1;
        if (f < 0) {
            err = "Fan not A, 1 to 5 or Q";
            break;
        }
        s = skipspace(s + 1);
        if (m->n && o <= m->outdoor[m->n - 1]) {
            err = "Outdoor temperatures not increasing";
            break;
        }
        m->outdoor[m->n] = o;
        m->fan[m->n] = f;
        m->n++;
        if (*s == ',') {
            s = skipspace(s + 1);
            if (!*s)
                err = "Trailing ,";
        } else if (*s)
            err = "Expected ,";
    }
    if (err)
        m->n = 0;
    return err;
}

int8_t heatctl_fanmap_fan(const heatctl_fanmap_t *m, float outdoor)
{
    if (!m->n || isnan(outdoor))
        return HEATCTL_FAN_LEAVE;
    int r = 0;
    for (int i = 1; i < m->n; i++)
        if (outdoor >= m->outdoor[i])
            r = i;
    return m->fan[r];
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

// Level for a demand (%): the highest level at or below it, plain 100 before 100q
static int level_for(const heatctl_table_t *t, int demand)
{
    int r = 0;
    for (int i = 0; i < t->n; i++)
        if (t->level[i].demand <= demand)
            r = i;
    return r;
}

// Highest level allowed, in the post-defrost hold or not
static int level_top(const heatctl_params_t *p, uint8_t hold)
{
    int top = p->table.n - 1;
    if (!p->demand_full) // No plain 100
        while (top > 0 && !p->table.level[top].quiet)
            top--;
    if (hold) // Capped at defrost.holdmax, no plain 100
        while (top > 0 && (p->table.level[top].demand > p->defrost_holdmax || !p->table.level[top].quiet))
            top--;
    return top;
}

// Highest level whose watts are at or below w, up to top
static int level_wanted(const heatctl_table_t *t, int top, float w)
{
    int r = 0;
    for (int i = 1; i <= top; i++)
        if (t->level[i].watts <= w)
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
    s->resumed = 0;
    if (state != s->state)
        s->fan_state = HEATCTL_OFF; // The new state's fan map applies at once
    s->state = state;
    s->since = now;
    if (sub != s->sub)
        s->sub_since = now; // A sub-state carried over (the post-defrost hold) keeps its time
    s->sub = sub;
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

// Heat per electrical watt of the table, roughly (demand.cop)
static float table_cop(const heatctl_params_t *p)
{
    return p->cop > 0 ? p->cop : 1;
}

static void enter_demand(heatctl_t *s, const heatctl_params_t *p, heatctl_out_t *out, uint32_t now, int demand)
{ // DEMAND with the integral at the heat of this demand's level, and no heat deficit
    enter(s, out, now, HEATCTL_DEMAND, keep_hold(s));
    s->integral = p->table.level[level_for(&p->table, demand)].watts * table_cop(p);
    s->deficit = s->debt = 0;
    s->level = -1; // Set at once
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

// Fan for the state: its fixed setting, or from its map, a fan from the map then kept for fan_hold at least
static int8_t state_fan(heatctl_t *s, const heatctl_params_t *p, int8_t fan, const heatctl_fanmap_t *map, float outdoor,
                        uint32_t now)
{
    if (fan != HEATCTL_FAN_MAP) {
        s->fan_state = HEATCTL_OFF;
        return fan;
    }
    int8_t want = heatctl_fanmap_fan(map, outdoor);
    if (want == HEATCTL_FAN_LEAVE) // No map, or outdoor unknown: keep what there is
        return s->fan_state == s->state ? s->fan : HEATCTL_FAN_LEAVE;
    if (s->fan_state != s->state || (want != s->fan && now - s->fan_at >= p->fan_hold &&
                                     s->sub != HEATCTL_SUB_DEFROST && s->sub != HEATCTL_SUB_HOLD)) {
        s->fan = want;
        s->fan_at = now;
        s->fan_state = s->state;
    }
    return s->fan;
}

// CRC-32 (as zlib's) of the kept block, all but the CRC itself
static uint32_t keep_crc(const heatctl_keep_t *k)
{
    const uint8_t *b = (const uint8_t *)k;
    uint32_t c = 0xFFFFFFFF;
    for (size_t n = offsetof(heatctl_keep_t, crc); n--;) {
        c ^= *b++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

// Age of the kept block now (s), UINT32_MAX if past counting
static uint32_t keep_age(const heatctl_t *s, uint32_t now)
{
    uint32_t a = s->keep_age + (now - s->keep_at);
    return a < s->keep_age ? UINT32_MAX : a;
}

void heatctl_resume(heatctl_t *s, const heatctl_keep_t *k, uint32_t clock, uint32_t now)
{
    s->kept = *k;
    s->keep_pending = 1;
    s->keep_age = clock >= k->at ? clock - k->at : UINT32_MAX; // A clock gone back has restarted
    s->keep_at = now;
}

void heatctl_keep(const heatctl_t *s, const heatctl_params_t *p, heatctl_keep_t *k, uint32_t clock)
{
    if (s->keep_pending) // Still to be tried: leave it, over another restart too
        return;
    memset(k, 0, sizeof(*k)); // Nothing kept
    if (!(s->state == HEATCTL_DEMAND && s->level >= 0 && s->level < p->table.n) && s->state != HEATCTL_COLD &&
        s->state != HEATCTL_IDLE)
        return;
    k->magic = HEATCTL_KEEP_MAGIC;
    k->version = HEATCTL_KEEP_VERSION;
    k->at = clock;
    k->state = s->state;
    if (s->state == HEATCTL_IDLE && s->sub == HEATCTL_SUB_REST)
        k->rest = s->resume;
    if (s->state == HEATCTL_DEMAND) {
        k->demand = p->table.level[s->level].demand; // Not the index, which a changed table would misplace
        k->quiet = p->table.level[s->level].quiet;
    }
    k->integral = s->integral;
    k->scale = s->scale;
    k->deficit = s->deficit;
    k->debt = s->debt;
    k->setpoint = s->setpoint;
    k->cold_integral = s->cold_integral;
    k->crc = keep_crc(k);
}

// Why the kept block may not be resumed now, "" if there is none, NULL if it may. *level is the level for DEMAND.
static const char *keep_check(const heatctl_t *s, const heatctl_params_t *p, uint32_t now, int *level)
{
    const heatctl_keep_t *k = &s->kept;
    if (!s->keep_pending || k->magic != HEATCTL_KEEP_MAGIC)
        return "";
    if (k->version != HEATCTL_KEEP_VERSION)
        return "other version";
    if (k->crc != keep_crc(k))
        return "bad checksum";
    if (keep_age(s, now) > HEATCTL_KEEP_AGE)
        return "too old";
    if (!isfinite(k->integral) || !isfinite(k->scale) || !isfinite(k->deficit) || !isfinite(k->debt) ||
        !isfinite(k->setpoint) || !isfinite(k->cold_integral))
        return "bad value";
    float cop = table_cop(p);
    if (k->scale && (k->scale < cop / 2 || k->scale > cop * 2)) // As the DEMAND step clamps it, 0 if not learnt yet
        return "bad value";
    switch (k->state) {
    case HEATCTL_DEMAND: {
        const heatctl_table_t *t = &p->table;
        *level = -1;
        for (int i = 0; i < t->n; i++)
            if (t->level[i].demand == k->demand && t->level[i].quiet == k->quiet)
                *level = i;
        if (*level < 0)
            return "level not in demand.table";
        if (k->integral < t->level[0].watts * cop || k->integral > t->level[level_top(p, 0)].watts * cop)
            return "bad value";
        return NULL;
    }
    case HEATCTL_COLD:
        if (k->setpoint < p->tmin || k->setpoint > p->tmax || k->cold_integral < p->tmin || k->cold_integral > p->tmax)
            return "bad value";
        return NULL;
    case HEATCTL_IDLE:
        if (k->rest && k->rest != HEATCTL_DEMAND && k->rest != HEATCTL_COLD)
            return "bad state";
        return NULL;
    default:
        return "bad state";
    }
}

// Resume the kept block, which keep_check has passed. Its timers start as on entering the state.
static void keep_resume(heatctl_t *s, heatctl_out_t *out, uint32_t now, int level)
{
    const heatctl_keep_t *k = &s->kept;
    s->scale = k->scale;
    if (k->state == HEATCTL_DEMAND) {
        enter(s, out, now, HEATCTL_DEMAND, HEATCTL_SUB_NONE);
        s->integral = s->heatw = k->integral;
        s->deficit = k->deficit;
        s->debt = k->debt;
        s->level = level;
        s->level_at = now;
        reason(out, "resumed after restart, demand %u%s, integral %.0f W", k->demand, k->quiet ? " QO" : "",
               k->integral);
    } else if (k->state == HEATCTL_COLD) {
        enter(s, out, now, HEATCTL_COLD, HEATCTL_SUB_NONE);
        s->cold_integral = k->cold_integral;
        s->setpoint = k->setpoint;
        s->setpoint_at = now;
        reason(out, "resumed after restart, setpoint %.1f, integral %.1f", k->setpoint, k->cold_integral);
    } else {
        enter(s, out, now, HEATCTL_IDLE, k->rest ? HEATCTL_SUB_REST : HEATCTL_SUB_NONE);
        s->resume = k->rest;
        reason(out, "resumed after restart%s", k->rest ? ", rest after defrost loop" : "");
    }
    s->resumed = 1;
}

heatctl_out_t heatctl_step(heatctl_t *s, const heatctl_params_t *p, const heatctl_in_t *in, uint32_t now)
{
    heatctl_out_t out = {0};
    uint32_t dt = (s->ticked && now > s->last) ? now - s->last : 0;
    if (dt > HEATCTL_DT_MAX)
        dt = HEATCTL_DT_MAX; // A stall (e.g. comms restart) is integrated as HEATCTL_DT_MAX at most
    s->ticked = 1;
    s->last = now;

    // Room, smoothed, restarted after a gap
    s->room_fresh = !isnan(in->room);
    if (s->room_fresh) {
        if (!s->room_ok || !p->smooth || now - s->room_at > p->smooth)
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
                if (p->defrost_watchdog && n > p->defrost_loops)
                    defrost_rest = n;
                else
                    enter_sub(s, &out, now, p->defrost_hold ? HEATCTL_SUB_HOLD : HEATCTL_SUB_NONE);
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
    } else
        s->armed = 0; // Not heating, so the coil may cool; heating has to be established again

    // Outdoor, smoothed, frozen during defrost and the hold after it (the unit's reading jumps)
    if (!isnan(in->outdoor) && s->sub != HEATCTL_SUB_DEFROST && s->sub != HEATCTL_SUB_HOLD) {
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
    out.deficit = out.heattarget = out.scale = NAN;
    out.fan = HEATCTL_FAN_LEAVE;

    if (!in->enabled || !in->power || !in->heat) {
        // Not tried while the unit's state is not known yet after a boot, but switched off, or too old by now, it is
        // not resumed, and is written over from now on
        if (!in->enabled || keep_age(s, now) > HEATCTL_KEEP_AGE)
            s->keep_pending = 0;
        if (s->state != HEATCTL_OFF) {
            enter(s, &out, now, HEATCTL_OFF, HEATCTL_SUB_NONE);
            reason(&out, "%s", !in->enabled ? "disabled" : !in->power ? "unit off" : "unit not heating");
        }
        out.state = HEATCTL_OFF;
        out.integral = s->integral;
        return out;
    }

    if (s->state == HEATCTL_OFF) { // Fresh start, or the first after a boot resumes the state kept over the restart
        s->sub = HEATCTL_SUB_NONE;
        s->armed = (!isnan(in->liquid) && in->liquid >= p->defrost_exit);
        s->defrosts = 0;
        if (!s->room_fresh)
            s->room_at = now; // Stale counts from the start
        int level = -1;
        const char *why = keep_check(s, p, now, &level);
        s->keep_pending = 0; // Tried once only
        if (!why)
            keep_resume(s, &out, now, level);
        else {
            enter_demand(s, p, &out, now, p->demand_start);
            reason(&out, "start%s%s", *why ? ", not resumed: " : "", why);
        }
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
            if (room > target + p->idle_over && now - s->since >= p->demand_minrun && s->level == 0) {
                // Only from the lowest level: above it, an overshoot just steps demand down
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
                        enter_demand(s, p, &out, now, p->table.level[0].demand);
                    reason(&out, "defrost rest over");
                }
            } else if (room < target - p->idle_under && now - s->since >= p->idle_min) {
                enter_demand(s, p, &out, now, p->table.level[0].demand);
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
        float cop = table_cop(p);
        int top = level_top(p, noraise);
        int clamp = level_top(p, 0); // The hold caps the level, not the integral
        int was = s->level;
        // Heat of the levels: the table has the shape, and the heat the unit has given of late per watt of the table
        // the scale, starting from demand.cop (and that alone where the heat output is not known)
        if (!(s->scale > 0))
            s->scale = cop;
        if (!isnan(in->heatout) && s->level >= 0 && s->sub == HEATCTL_SUB_NONE && !stale)
            s->scale += (in->heatout / t->level[s->level].watts - s->scale) * dt / (HEATCTL_SCALE_SMOOTH + dt);
        s->scale = clampf(s->scale, cop / 2, cop * 2);
        float scale = isnan(in->heatout) ? cop : s->scale;
        if (!freeze) { // PI, the integral in W of heat, with anti-windup where the top or bottom level is not enough
            float di = p->ti > 0 ? p->kp * e * dt / p->ti : 0; // ti 0: no integral, P only
            if (di > 0 && s->level >= clamp && t->level[clamp].watts * scale <= s->heatw)
                di = 0;
            if (di < 0 && s->level <= 0 && t->level[0].watts * scale >= s->heatw)
                di = 0;
            s->integral = clampf(s->integral + di, t->level[0].watts * cop, t->level[clamp].watts * cop);
        }
        if (!stale)
            s->heatw = p->kp * e + s->integral;
        else if (s->level < 0)
            s->heatw = s->integral; // Starting with no reading
        // Heat target: the heat request, and what the last defrosts cost, which is asked for over the next hours,
        // not at once
        float heattarget = s->heatw + s->debt * 3600 / HEATCTL_DEFROST_SPREAD;
        float heat = s->level < 0 ? 0 : t->level[s->level].watts * scale; // What this level gives
        // A step is due once the heat deficit (or surplus) is half of what the next level would give over the heat
        // target in the shortest time demand may stay there: from then on the step leaves less of a deficit or
        // surplus than staying does
        float pulse = p->demand_rate / 3600.0f / 2;
        if (s->level >= 0 && !stale) {
            // Heat deficit: the heat request less the heat the unit gave (the table's where that is not known)
            float got = !isnan(in->heatout)             ? in->heatout
                        : s->sub == HEATCTL_SUB_DEFROST ? 0
                                                        : t->level[s->level].watts * cop;
            float d = (s->heatw - got) * dt / 3600;
            if (d > 0 && s->sub != HEATCTL_SUB_NONE)
                s->debt += d; // Lost to a defrost or the hold after it
            else
                s->deficit += d;
            d = s->debt * dt / (HEATCTL_DEFROST_SPREAD + dt);
            s->debt -= d;
            s->deficit += d;
            if (s->level >= clamp && s->deficit > fmaxf(heat - heattarget, 0) * pulse)
                s->deficit = fmaxf(heat - heattarget, 0) * pulse; // No level above to make more up with
            if (s->level <= 0 && s->deficit < -fmaxf(heattarget - heat, 0) * pulse)
                s->deficit = -fmaxf(heattarget - heat, 0) * pulse;
        }
        if (s->level < 0) // Just entered: by the table
            s->level = level_wanted(t, top, s->heatw / cop);
        else if (s->level > top)
            s->level = top; // Cap at once, also in defrost (e.g. a shorter table)
        else if (s->sub == HEATCTL_SUB_DEFROST)
            ; // Hold
        else if (stale)
            ; // Hold
        else if (now - s->level_at >= p->demand_rate) {
            // Up only from a level that gives too little, and down only from one that gives too much, so that heat
            // still on its way after a step does not bring on the next. The step goes on over levels that are still
            // short of the heat target, or still over it
            if (s->level < top && !noraise && heat < heattarget &&
                s->deficit >= fmaxf(t->level[s->level + 1].watts * scale - heattarget, 0) * pulse)
                do
                    s->level++;
                while (s->level < top && t->level[s->level + 1].watts * scale <= heattarget);
            else if (s->level > 0 && heat > heattarget &&
                     s->deficit <= -fmaxf(heattarget - t->level[s->level - 1].watts * scale, 0) * pulse)
                do
                    s->level--;
                while (s->level > 0 && t->level[s->level - 1].watts * scale >= heattarget);
        }
        if (s->level != was)
            s->level_at = now;
        out.demand = t->level[s->level].demand;
        out.quiet = t->level[s->level].quiet || s->sub == HEATCTL_SUB_DEFROST;
        out.setpoint = p->demand_set;
        out.fan = state_fan(s, p, p->demand_fan, &p->demand_fanmap, outdoor, now);
        out.heatw = s->heatw;
        out.integral = s->integral;
        out.deficit = s->deficit;
        out.heattarget = heattarget;
        out.scale = scale;
        break;
    }
    case HEATCTL_IDLE:
        out.demand = p->table.level[0].demand;
        out.quiet = 1;
        out.setpoint = p->idle_temp;
        if (s->sub == HEATCTL_SUB_REST)
            out.fan = 0; // Auto for the watchdog rest, whatever idle.fan is
        else
            out.fan = state_fan(s, p, p->idle_fan, &p->idle_fanmap, outdoor, now);
        out.integral = s->integral;
        break;
    case HEATCTL_COLD:
        if (!freeze) { // PI on the setpoint, the integral in °C
            float di = p->cold_ti > 0 ? p->cold_kp * e * dt / p->cold_ti : 0;
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
        out.fan = state_fan(s, p, p->cold_fan, &p->cold_fanmap, outdoor, now);
        out.integral = s->cold_integral;
        for (int i = p->table.n - 1; i >= 0; i--)
            if (!p->table.level[i].quiet) {
                out.heatw = p->table.level[i].watts * table_cop(p);
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
    out.resumed = s->resumed && now - s->since < HEATCTL_RESUMED_SHOW;
    out.state = s->state;
    out.sub = s->sub;
    return out;
}
