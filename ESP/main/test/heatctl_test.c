// Host unit test for the heating controller (heatctl.c)
// Build and run with: make -C ESP test

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../heatctl.h"

static int failed = 0, passed = 0;

#define CHECK(name, cond)                         \
    do {                                          \
        if (cond)                                 \
            passed++;                             \
        else {                                    \
            printf("FAIL %s: %s\n", name, #cond); \
            failed++;                             \
        }                                         \
    } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 0.01)

// Settings defaults (settings.def), in heatctl units
static heatctl_params_t params(void)
{
    heatctl_params_t p = {
        .target = 21,
        .stale = 1800,
        .smooth = 300,
        .tmin = 16,
        .tmax = 32,
        .demand_set = 31,
        .demand_full = 1,
        .demand_start = 60,
        .demand_rate = 600,
        .demand_minrun = 30 * 60,
        .kp = 300,
        .ti = 90 * 60,
        .idle_temp = 16,
        .idle_over = 0.5,
        .idle_under = 0,
        .idle_min = 30 * 60,
        .cold_enter = -15,
        .cold_exit = -10,
        .cold_hold = 15 * 60,
        .cold_set = 23,
        .cold_rate = 600,
        .cold_kp = 2,
        .cold_ti = 120 * 60,
        .defrost_enter = 20,
        .defrost_exit = 25,
        .defrost_max = 30 * 60,
        .defrost_hold = 10 * 60,
        .defrost_loops = 3,
        .defrost_window = 90 * 60,
        .defrost_rest = 15 * 60,
        .idle_fan = HEATCTL_FAN_LEAVE,
        .demand_fan = HEATCTL_FAN_LEAVE,
        .cold_fan = HEATCTL_FAN_LEAVE,
    };
    heatctl_parse_table(&p.table, "");
    return p;
}

// Unit on in heat, room at target, mild outside, liquid hot (heating)
static heatctl_in_t inputs(void)
{
    return (heatctl_in_t){.enabled = 1, .power = 1, .heat = 1, .room = 21, .outdoor = 5, .liquid = 35};
}

typedef struct {
    heatctl_t s;
    heatctl_params_t p;
    heatctl_in_t in;
    heatctl_out_t out;
    uint32_t now;
    int changes; // State or sub-state changes seen by run()
} sim_t;

static void start(sim_t *x)
{
    memset(x, 0, sizeof(*x));
    x->p = params();
    x->in = inputs();
    x->now = 1000;
    x->out = heatctl_step(&x->s, &x->p, &x->in, x->now);
}

// Step once a second for secs seconds
static void run(sim_t *x, uint32_t secs)
{
    while (secs--) {
        x->now++;
        x->out = heatctl_step(&x->s, &x->p, &x->in, x->now);
        if (x->out.changed)
            x->changes++;
    }
}

// Step until the state is st (or sub-state sub if not -1), at most secs seconds. Returns seconds taken, or -1.
static int run_until(sim_t *x, heatctl_state_t st, int sub, uint32_t secs)
{
    for (uint32_t i = 1; i <= secs; i++) {
        run(x, 1);
        if (x->out.state == st && (sub < 0 || (int)x->out.sub == sub))
            return i;
    }
    return -1;
}

static void test_table(void)
{
    heatctl_table_t t;
    CHECK("table default ok", !heatctl_parse_table(&t, HEATCTL_TABLE_DEFAULT));
    CHECK("table default rungs", t.n == 9);
    CHECK("table first", t.rung[0].demand == 40 && NEAR(t.rung[0].watts, 220) && t.rung[0].quiet);
    CHECK("table 100q", t.rung[7].demand == 100 && t.rung[7].quiet && NEAR(t.rung[7].watts, 1510));
    CHECK("table plain 100 has no QO", t.rung[8].demand == 100 && !t.rung[8].quiet && NEAR(t.rung[8].watts, 2470));
    int qo = 1;
    for (int i = 0; i < 8; i++)
        qo &= t.rung[i].quiet;
    CHECK("table QO on every rung but plain 100", qo);
    CHECK("table empty is default", !heatctl_parse_table(&t, "") && t.n == 9);
    CHECK("table custom", !heatctl_parse_table(&t, " 30:100, 60:300 ,100:900") && t.n == 3 && t.rung[0].demand == 30 &&
                              NEAR(t.rung[2].watts, 900) && !t.rung[2].quiet && t.rung[1].quiet);
    const char *bad[] = {"40:220,50", "40:220,x:300",  "40:220,50:200",
                         "20:100",    "101:900",       "40:220,50:280q",
                         "40:-5",     "40:220;50:300", "1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12,13:13",
                         "40:220,"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        heatctl_parse_table(&t, "30:1,100:2"); // Something else first
        char name[80];
        snprintf(name, sizeof(name), "table bad \"%s\"", bad[i]);
        CHECK(name, heatctl_parse_table(&t, bad[i]) && t.n == 9 && NEAR(t.rung[8].watts, 2470));
    }
}

static void test_curve(void)
{
    heatctl_curve_t c;
    CHECK("curve ok", !heatctl_parse_curve(&c, "-25:1.0,-10:0.5,0:0") && c.n == 3);
    CHECK("curve flat below", NEAR(heatctl_curve_offset(&c, -30), 1.0));
    CHECK("curve between", NEAR(heatctl_curve_offset(&c, -17.5), 0.75));
    CHECK("curve between 2", NEAR(heatctl_curve_offset(&c, -5), 0.25));
    CHECK("curve flat above", NEAR(heatctl_curve_offset(&c, 10), 0));
    CHECK("curve unknown outdoor", NEAR(heatctl_curve_offset(&c, NAN), 0));
    CHECK("curve empty", !heatctl_parse_curve(&c, "") && c.n == 0 && NEAR(heatctl_curve_offset(&c, -20), 0));
    CHECK("curve one point", !heatctl_parse_curve(&c, "-5:0.3") && NEAR(heatctl_curve_offset(&c, 9), 0.3));
    const char *bad[] = {
        "x", "-10:1,-20:2", "-10:1,-10:2", "-10:", "-10:1,", "-10 1", "1:1,2:1,3:1,4:1,5:1,6:1,7:1,8:1,9:1"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char name[80];
        snprintf(name, sizeof(name), "curve bad \"%s\"", bad[i]);
        CHECK(name, heatctl_parse_curve(&c, bad[i]) && c.n == 0);
    }
}

static void test_off_and_start(void)
{
    sim_t x;
    start(&x);
    CHECK("start: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_NONE && x.out.changed);
    CHECK("start: integral at demand.start rung", NEAR(x.out.integral, 350));
    CHECK("start: demand 60 with QO", x.out.demand == 60 && x.out.quiet);
    CHECK("start: setpoint demand.set", NEAR(x.out.setpoint, 31));
    CHECK("start: target", NEAR(x.out.target, 21));
    CHECK("start: heat wanted", NEAR(x.out.heatw, 350));
    CHECK("start: fan left alone", x.out.fan == HEATCTL_FAN_LEAVE);

    x.in.heat = 0;
    run(&x, 1);
    CHECK("not heating: OFF", x.out.state == HEATCTL_OFF && x.out.changed);
    x.in.heat = 1;
    x.in.power = 0;
    run(&x, 1);
    CHECK("unit off: OFF", x.out.state == HEATCTL_OFF);
    x.in.power = 1;
    x.in.enabled = 0;
    run(&x, 1);
    CHECK("disabled: OFF", x.out.state == HEATCTL_OFF);

    // Restart is fresh, even after the integral has moved
    x.in.enabled = 1;
    x.in.room = 19;
    run(&x, 1);
    run(&x, 3600);
    CHECK("integral grew", x.out.integral > 400);
    x.in.enabled = 0;
    run(&x, 1);
    x.in.enabled = 1;
    run(&x, 1);
    CHECK("restart: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.changed);
    CHECK("restart: fresh integral", NEAR(x.out.integral, 350));
}

static void test_demand_idle(void)
{
    sim_t x;
    start(&x);
    x.in.room = 21.6;
    int t = run_until(&x, HEATCTL_IDLE, -1, 4000);
    CHECK("DEMAND -> IDLE after demand.minrun", t == 1800);
    CHECK("IDLE reason", strstr(x.out.reason, "room") != NULL);
    CHECK("IDLE: setpoint idle.temp", NEAR(x.out.setpoint, 16));
    CHECK("IDLE: lowest rung with QO", x.out.demand == 40 && x.out.quiet);

    start(&x);
    x.in.room = 21.5; // Exactly target + idle.over
    run(&x, 3 * 3600);
    CHECK("room at target + idle.over stays DEMAND", x.out.state == HEATCTL_DEMAND && x.changes == 0);

    // IDLE -> DEMAND
    start(&x);
    x.in.room = 21.6;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    x.in.room = 20.9;
    t = run_until(&x, HEATCTL_DEMAND, -1, 4000);
    CHECK("IDLE -> DEMAND after idle.min", t == 1800);
    CHECK("after IDLE: integral at lowest rung", NEAR(x.out.integral, 220));
    CHECK("after IDLE: setpoint demand.set", NEAR(x.out.setpoint, 31));

    start(&x);
    x.in.room = 21.6;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    x.in.room = 21.0; // Exactly target - idle.under
    run(&x, 3 * 3600);
    CHECK("room at target - idle.under stays IDLE", x.out.state == HEATCTL_IDLE);

    // Room drops quickly but IDLE has not lasted idle.min
    start(&x);
    x.in.room = 21.6;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    x.in.room = 19;
    run(&x, 1799);
    CHECK("IDLE holds for idle.min", x.out.state == HEATCTL_IDLE);
    run(&x, 1);
    CHECK("IDLE ends at idle.min", x.out.state == HEATCTL_DEMAND);
}

static void test_cold(void)
{
    sim_t x;
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now); // Fresh start with outdoor already -20
    CHECK("outdoor smoothing starts at first reading", NEAR(x.out.outdoor, -20));
    int t = run_until(&x, HEATCTL_COLD, -1, 4000);
    CHECK("DEMAND -> COLD after cold.hold", t == 900);
    CHECK("COLD: demand 100 QO off", x.out.demand == 100 && !x.out.quiet);
    CHECK("COLD: setpoint starts at cold.set", NEAR(x.out.setpoint, 23));
    CHECK("COLD reason", strstr(x.out.reason, "outdoor") != NULL);

    // COLD -> DEMAND: cold.hold after smoothed outdoor goes above cold.exit
    x.in.outdoor = -5;
    uint32_t crossed = 0;
    int left = 0;
    for (int i = 0; i < 4 * 3600 && !left; i++) {
        run(&x, 1);
        if (!crossed && x.out.outdoor > -10)
            crossed = x.now;
        if (x.out.state != HEATCTL_COLD)
            left = 1;
    }
    CHECK("COLD -> DEMAND", left && x.out.state == HEATCTL_DEMAND);
    CHECK("COLD -> DEMAND after cold.hold above cold.exit", crossed && x.now - crossed == 900);
    CHECK("after COLD: integral at 95% rung", NEAR(x.out.integral, 640));

    // Unknown outdoor never enters COLD
    start(&x);
    x.in.outdoor = NAN;
    run(&x, 3 * 3600);
    CHECK("unknown outdoor stays DEMAND", x.out.state == HEATCTL_DEMAND);

    // Cold spell shorter than cold.hold
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run(&x, 500);
    x.in.outdoor = 20; // Smoothed rises back above -15 within cold.hold
    run(&x, 3 * 3600);
    CHECK("short cold spell stays DEMAND", x.out.state == HEATCTL_DEMAND);

    // COLD setpoint PI: steps of 0.5 at most once per cold.rate, clamped to t.max
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.in.room = 21;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    x.in.room = 18;
    float sp = x.out.setpoint;
    uint32_t at = x.now;
    int ok = 1, steps = 0;
    for (int i = 0; i < 6 * 3600; i++) {
        run(&x, 1);
        if (x.out.setpoint != sp) {
            if (fabsf(x.out.setpoint - sp) > 0.5f + 1e-4f || x.now - at < 600)
                ok = 0;
            if (fmodf(x.out.setpoint * 2, 1) != 0)
                ok = 0;
            sp = x.out.setpoint;
            at = x.now;
            steps++;
        }
    }
    CHECK("COLD setpoint steps of 0.5 per cold.rate", ok && steps >= 5);
    CHECK("COLD setpoint clamped to t.max", NEAR(x.out.setpoint, 32));
    CHECK("COLD integral clamped to t.max", x.out.integral <= 32.0f + 1e-4f);
    // Room warm: setpoint comes down, no IDLE from COLD
    x.in.room = 24;
    run(&x, 6 * 3600);
    CHECK("COLD never goes IDLE", x.out.state == HEATCTL_COLD);
    CHECK("COLD setpoint comes down", x.out.setpoint < 23);
    CHECK("COLD setpoint clamped to t.min", x.out.setpoint >= 16);
}

static void test_defrost(void)
{
    sim_t x;
    start(&x);
    run(&x, 100);
    x.in.room = 20;
    x.in.liquid = 15;
    run(&x, 1);
    CHECK("defrost entered", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_DEFROST && x.out.changed);
    float integral = x.out.integral;
    run(&x, 1000);
    CHECK("defrost holds demand", x.out.demand == 60);
    CHECK("defrost QO on", x.out.quiet);
    CHECK("defrost holds setpoint", NEAR(x.out.setpoint, 31));
    CHECK("defrost freezes integral", x.out.integral == integral);
    x.in.liquid = 26;
    run(&x, 1);
    CHECK("defrost exit on liquid", x.out.sub == HEATCTL_SUB_HOLD && x.out.changed && x.out.resend_fan);
    run(&x, 1);
    CHECK("fan resend only once", !x.out.resend_fan);
    integral = x.out.integral;
    run(&x, 598);
    CHECK("hold: integral may not rise", x.out.integral <= integral);
    CHECK("hold: still in hold", x.out.sub == HEATCTL_SUB_HOLD);
    run(&x, 1);
    CHECK("hold ends after defrost.hold", x.out.sub == HEATCTL_SUB_NONE && x.out.changed);
    run(&x, 600);
    CHECK("integral rises again after hold", x.out.integral > integral);

    // Cap
    start(&x);
    x.in.liquid = 15;
    run(&x, 1);
    int t = run_until(&x, HEATCTL_DEMAND, HEATCTL_SUB_HOLD, 4000);
    CHECK("defrost capped at defrost.max", t == 1800);
    CHECK("cap reason", strstr(x.out.reason, "cap") != NULL);
    run(&x, 3600);
    CHECK("no new defrost until liquid has been hot again", x.out.sub != HEATCTL_SUB_DEFROST);

    // Liquid low from the start (unit not heating yet): not a defrost
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.liquid = 15;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run(&x, 3600);
    CHECK("cold liquid at start is not a defrost", x.out.sub == HEATCTL_SUB_NONE && x.changes == 0);

    // Unknown liquid never enters defrost
    start(&x);
    x.in.liquid = NAN;
    run(&x, 3600);
    CHECK("unknown liquid is not a defrost", x.out.sub == HEATCTL_SUB_NONE && x.changes == 0);

    // No IDLE during defrost
    start(&x);
    run(&x, 1);
    x.in.liquid = 15;
    x.in.room = 22.5;
    run(&x, 1799);
    CHECK("no IDLE during defrost", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_DEFROST);
    x.in.liquid = 30;
    run(&x, 1);
    CHECK("IDLE once defrost ends", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_NONE);

    // No COLD during defrost, and outdoor frozen during defrost
    start(&x);
    run(&x, 1);
    x.in.liquid = 15;
    float outdoor = x.out.outdoor;
    x.in.outdoor = -25; // The unit's reading jumps during defrost
    run(&x, 1700);
    CHECK("outdoor frozen during defrost", x.out.outdoor == outdoor);
    CHECK("no COLD during defrost", x.out.state == HEATCTL_DEMAND);

    // COLD defrost: demand 100 QO off held
    memset(&x.s, 0, sizeof(x.s));
    x.in = inputs();
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    x.in.liquid = 10;
    run(&x, 1);
    CHECK("COLD defrost", x.out.state == HEATCTL_COLD && x.out.sub == HEATCTL_SUB_DEFROST);
    CHECK("COLD defrost: demand 100 QO off", x.out.demand == 100 && !x.out.quiet);
}

static void test_hold_cap(int full)
{
    sim_t x;
    start(&x);
    x.p.demand_full = full;
    x.in.room = 18;
    run(&x, 5 * 3600);
    if (full)
        CHECK("cold room reaches plain 100", x.out.demand == 100 && !x.out.quiet);
    else
        CHECK("demand.full off tops out at 100q", x.out.demand == 100 && x.out.quiet);
    CHECK("anti-windup: integral at most top rung", x.out.integral <= (full ? 2470 : 1510) + 1e-3f);
    x.in.liquid = 15;
    run(&x, 1);
    CHECK("defrost at 100: QO on", x.out.sub == HEATCTL_SUB_DEFROST && x.out.demand == 100 && x.out.quiet);
    x.in.liquid = 30;
    run(&x, 1);
    CHECK("hold caps demand at 95", x.out.sub == HEATCTL_SUB_HOLD && x.out.demand == 95 && x.out.quiet);
    run(&x, 598);
    CHECK("hold keeps cap", x.out.demand == 95);
    run(&x, 2);
    run(&x, 600);
    CHECK("after hold back to top", x.out.demand == 100 && x.out.quiet == !full);
}

static void test_watchdog(int defrosts)
{
    sim_t x;
    start(&x);
    for (int i = 0; i < defrosts; i++) {
        run(&x, 600);
        x.in.liquid = 15; // 5 min defrost
        run(&x, 300);
        x.in.liquid = 35;
        run(&x, 1);
    }
    if (defrosts <= 3) {
        CHECK("3 defrosts in window: no rest", x.out.state == HEATCTL_DEMAND);
        return;
    }
    CHECK("4 defrosts in window: IDLE rest", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_REST);
    CHECK("rest reason", strstr(x.out.reason, "defrost") != NULL);
    CHECK("rest: setpoint idle.temp", NEAR(x.out.setpoint, 16));
    int t = run_until(&x, HEATCTL_DEMAND, -1, 4000);
    CHECK("rest lasts defrost.rest", t == 900);
    CHECK("after rest: integral at lowest rung", NEAR(x.out.integral, 220));
}

static void test_watchdog_slow(void)
{
    sim_t x;
    start(&x);
    for (int i = 0; i < 6; i++) { // A defrost every 40 min: never more than 3 in 90 min
        run(&x, 2100);
        x.in.liquid = 15;
        run(&x, 300);
        x.in.liquid = 35;
        run(&x, 1);
    }
    CHECK("defrosts spread out: no rest", x.out.state == HEATCTL_DEMAND);
}

static void test_stale(void)
{
    sim_t x;
    start(&x);
    run(&x, 100);
    x.in.room = 19; // Heat wanted would rise
    run(&x, 50);
    x.in.room = NAN;
    run(&x, 1);
    float integral = x.out.integral;
    uint8_t demand = x.out.demand;
    run(&x, 1798);
    CHECK("stale: integral frozen", x.out.integral == integral);
    CHECK("stale: demand held", x.out.demand == demand);
    CHECK("stale: still DEMAND before auto.stale", x.out.state == HEATCTL_DEMAND);
    run(&x, 1);
    CHECK("NOSENSOR after auto.stale", x.out.state == HEATCTL_NOSENSOR && x.out.changed);
    CHECK("NOSENSOR: setpoint target, demand 100, QO off",
          NEAR(x.out.setpoint, 21) && x.out.demand == 100 && !x.out.quiet);
    CHECK("NOSENSOR: fan left alone", x.out.fan == HEATCTL_FAN_LEAVE);
    x.in.room = 22;
    run(&x, 1);
    CHECK("reading back: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.changed);
    CHECK("reading back: fresh integral", NEAR(x.out.integral, 350));

    // No reading at all from the start
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.room = NAN;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    CHECK("no reading at start: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.demand == 60);
    int t = run_until(&x, HEATCTL_NOSENSOR, -1, 4000);
    CHECK("no reading at start: NOSENSOR after auto.stale", t == 1800);

    // NOSENSOR setpoint includes the curve, from IDLE too
    start(&x);
    heatctl_parse_curve(&x.p.curve, "-10:1,10:0");
    x.in.room = 23;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    x.in.room = NAN;
    run_until(&x, HEATCTL_NOSENSOR, -1, 4000);
    CHECK("NOSENSOR from IDLE", x.out.state == HEATCTL_NOSENSOR);
    CHECK("NOSENSOR setpoint after curve", NEAR(x.out.setpoint, 21.25) && NEAR(x.out.target, 21.25));
}

static void test_rate_and_rungs(void)
{
    sim_t x;
    start(&x);
    x.in.room = 19;
    run(&x, 599);
    CHECK("demand held for demand.rate", x.out.demand == 60);
    run(&x, 1);
    CHECK("one change may jump several rungs", x.out.demand == 95);
    uint8_t d = x.out.demand;
    uint32_t at = x.now;
    int ok = 1;
    x.in.room = 20.5;
    for (int i = 0; i < 8 * 3600; i++) {
        run(&x, 1);
        if (x.out.demand != d) {
            if (x.now - at < 600)
                ok = 0;
            d = x.out.demand;
            at = x.now;
        }
    }
    CHECK("demand changes at most once per demand.rate", ok);

    // Anti-windup at the bottom: warm room, kept in DEMAND
    start(&x);
    x.p.idle_over = 5;
    x.in.room = 23;
    run(&x, 6 * 3600);
    CHECK("warm room: lowest rung", x.out.demand == 40 && x.out.state == HEATCTL_DEMAND);
    CHECK("anti-windup: integral not below lowest rung", x.out.integral >= 220 - 1e-3f);
    x.in.room = 21;
    run(&x, 1200);
    CHECK("back at target: demand leaves the lowest rung", x.out.demand > 40);

    // Fan per state
    start(&x);
    x.p.demand_fan = 3;
    x.p.idle_fan = 6;
    x.p.cold_fan = 0;
    run(&x, 1);
    CHECK("DEMAND fan", x.out.fan == 3);
    x.in.room = 22;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    CHECK("IDLE fan", x.out.fan == 6);
    memset(&x.s, 0, sizeof(x.s));
    x.in = inputs();
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    CHECK("COLD fan", x.out.fan == 0);
}

// Electrical watts the unit draws for the controller's outputs
static float model_watts(const sim_t *x)
{
    if (x->out.state != HEATCTL_DEMAND || x->out.sub == HEATCTL_SUB_DEFROST)
        return 0; // IDLE: setpoint far below the room, so the unit stops
    for (int r = x->p.table.n - 1; r >= 0; r--)
        if (x->p.table.rung[r].demand == x->out.demand &&
            (x->p.table.rung[r].quiet == x->out.quiet || x->out.demand < 100))
            return x->p.table.rung[r].watts;
    return 0;
}

// Room model: first-order (one node, capacity C, loss UA x (room - outdoor), COP 3), or the two-node house from the
// heat-model PoC (room air 2 MJ/K, building mass 40 MJ/K, COP 3.3 + 0.07 x outdoor, 10 min heat pump lag, BLE in 0.1
// steps). Checks the loop settles without hunting between DEMAND and IDLE.
static void test_room_model(float outdoor, float capacity, int two_node)
{
    sim_t x;
    start(&x);
    x.in.outdoor = outdoor;
    float room = 21, mass = 19.5, q = 0;
    float cop = two_node ? 3.3f + 0.07f * outdoor : 3;
    int idles = 0;
    float lo = 99, hi = -99;
    for (uint32_t i = 0; i < 72 * 3600; i++) {
        float w = model_watts(&x);
        if (two_node) {
            q += (w * cop - q) / 600;
            float droom = (500 * (mass - room) + 40 * (outdoor - room) + q) / 2e6f;
            mass += (500 * (room - mass) + 60 * (outdoor - mass)) / 40e6f;
            room += droom;
            x.in.room = roundf(room * 10) / 10;
        } else {
            room += (cop * w - 150 * (room - outdoor)) / capacity;
            x.in.room = room;
        }
        run(&x, 1);
        if (i > 24 * 3600) { // Settled
            if (x.out.changed && x.out.state == HEATCTL_IDLE)
                idles++;
            if (room < lo)
                lo = room;
            if (room > hi)
                hi = room;
        }
    }
    char name[120];
    if (two_node)
        snprintf(name, sizeof(name), "two-node house outdoor %.0f C: %d IDLE, room %.2f..%.2f", outdoor, idles, lo, hi);
    else
        snprintf(name, sizeof(name), "first-order room outdoor %.0f C %.0f MJ/K: %d IDLE, room %.2f..%.2f", outdoor,
                 capacity / 1e6, idles, lo, hi);
    CHECK(name, idles == 0 && lo > 20.5f && hi < 21.5f);
    printf("%s\n", name);
}

static void test_ble_env(void)
{
    uint8_t from_ble = 0;
    float env = heatctl_ble_env(NAN, &from_ble, 21.5);
    CHECK("BLE reading used", env == 21.5f && from_ble);
    env = heatctl_ble_env(env, &from_ble, NAN);
    CHECK("BLE reading gone stale is unknown", isnan(env) && !from_ble);
    env = heatctl_ble_env(20.0, &from_ble, NAN);
    CHECK("MQTT reading kept without BLE", env == 20.0f && !from_ble);
}

int main(void)
{
    test_table();
    test_curve();
    test_off_and_start();
    test_demand_idle();
    test_cold();
    test_defrost();
    test_hold_cap(1);
    test_hold_cap(0);
    test_watchdog(3);
    test_watchdog(4);
    test_watchdog_slow();
    test_stale();
    test_rate_and_rungs();
    // First-order room, loads of 300, 700 and 1550 W
    test_room_model(15, 5e6, 0);
    test_room_model(7, 5e6, 0);
    test_room_model(-10, 5e6, 0);
    test_room_model(7, 15e6, 0);
    test_room_model(-12, 0, 1);
    test_room_model(-5, 0, 1);
    test_room_model(0, 0, 1);
    test_room_model(10, 0, 1);
    test_ble_env();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
