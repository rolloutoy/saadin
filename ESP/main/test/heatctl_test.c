// Host unit test for the heating controller (heatctl.c)
// Build and run with: make -C ESP test

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../heatctl.h"

#ifndef DEMAND_TABLE
#error DEMAND_TABLE (the demand.table default from settings.def) comes from the Makefile
#endif

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
        .demand_set = 30,
        .demand_full = 1,
        .demand_start = 60,
        .demand_rate = 600,
        .demand_minrun = 30 * 60,
        .cop = 3,
        .kp = 900,
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
        .defrost_enter = 5,
        .defrost_exit = 25,
        .defrost_max = 30 * 60,
        .defrost_hold = 10 * 60,
        .defrost_holdmax = 100,
        .defrost_watchdog = 0,
        .defrost_loops = 3,
        .defrost_window = 90 * 60,
        .defrost_rest = 15 * 60,
        .idle_fan = HEATCTL_FAN_LEAVE,
        .demand_fan = HEATCTL_FAN_LEAVE,
        .cold_fan = HEATCTL_FAN_LEAVE,
    };
    heatctl_parse_table(&p.table, "", DEMAND_TABLE);
    return p;
}

// Unit on in heat, room at target, mild outside, liquid hot (heating), heat output not known
static heatctl_in_t inputs(void)
{
    return (heatctl_in_t){.enabled = 1, .power = 1, .heat = 1, .room = 21, .outdoor = 5, .liquid = 35, .heatout = NAN};
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
    CHECK("table default ok", !heatctl_parse_table(&t, DEMAND_TABLE, DEMAND_TABLE));
    CHECK("table default levels", t.n == 11);
    CHECK("table first", t.level[0].demand == 40 && NEAR(t.level[0].watts, 270) && t.level[0].quiet);
    CHECK("table 100q", t.level[9].demand == 100 && t.level[9].quiet && NEAR(t.level[9].watts, 1510));
    CHECK("table plain 100 has no QO",
          t.level[10].demand == 100 && !t.level[10].quiet && NEAR(t.level[10].watts, 2470));
    int qo = 1;
    for (int i = 0; i < 10; i++)
        qo &= t.level[i].quiet;
    CHECK("table QO on every level but plain 100", qo);
    CHECK("table empty is default", !heatctl_parse_table(&t, "", DEMAND_TABLE) && t.n == 11);
    CHECK("table custom", !heatctl_parse_table(&t, " 30:100, 60:300 ,100:900", DEMAND_TABLE) && t.n == 3 &&
                              t.level[0].demand == 30 && NEAR(t.level[2].watts, 900) && !t.level[2].quiet &&
                              t.level[1].quiet);
    const char *bad[] = {"40:220,50", "40:220,x:300",  "40:220,50:200",
                         "20:100",    "101:900",       "40:220,50:280q",
                         "40:-5",     "40:220;50:300", "1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12,13:13",
                         "40:220,"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        heatctl_parse_table(&t, "30:1,100:2", DEMAND_TABLE); // Something else first
        char name[80];
        snprintf(name, sizeof(name), "table bad \"%s\"", bad[i]);
        CHECK(name, heatctl_parse_table(&t, bad[i], DEMAND_TABLE) && t.n == 11 && NEAR(t.level[10].watts, 2470));
    }
    CHECK("table bad uses the given default", heatctl_parse_table(&t, "x", "30:1,100:2") && t.n == 2);
    char buf[100];
    heatctl_parse_table(&t, "", DEMAND_TABLE);
    CHECK("table as text is the default again",
          heatctl_format_table(&t, buf, sizeof(buf)) == (int)strlen(DEMAND_TABLE) && !strcmp(buf, DEMAND_TABLE));
    heatctl_parse_table(&t, " 30:100, 60:300.5 ,100q:800,100:900", DEMAND_TABLE);
    CHECK("table as text",
          heatctl_format_table(&t, buf, sizeof(buf)) == 32 && !strcmp(buf, "30:100,60:300.5,100q:800,100:900"));
    CHECK("table as text, cut short", heatctl_format_table(&t, buf, 10) == 32 && !strcmp(buf, "30:100,60"));
    t.n = 0;
    CHECK("table as text, empty", heatctl_format_table(&t, buf, sizeof(buf)) == 0 && !*buf);
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
    CHECK("start: integral at the heat of the demand.start level", NEAR(x.out.integral, 1200));
    CHECK("start: demand 60 with QO", x.out.demand == 60 && x.out.quiet);
    CHECK("start: setpoint demand.set", NEAR(x.out.setpoint, 30));
    CHECK("start: target", NEAR(x.out.target, 21));
    CHECK("start: heat request", NEAR(x.out.heatw, 1200));
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
    CHECK("integral grew", x.out.integral > 1200);
    x.in.enabled = 0;
    run(&x, 1);
    x.in.enabled = 1;
    run(&x, 1);
    CHECK("restart: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.changed);
    CHECK("restart: fresh integral", NEAR(x.out.integral, 1200));
}

// IDLE is for when even the lowest level is too much: an overshoot at a higher level steps demand down, not off
static void test_idle_only_from_lowest(void)
{
    sim_t x;
    start(&x);
    x.p.demand_full = 0;
    x.in.room = 18;
    run(&x, 5 * 3600);
    CHECK("idle from lowest: cold room at 100q", x.out.demand == 100 && x.out.quiet);
    x.in.room = 21.6; // A burst of warm air, over target + idle.over, long after demand.minrun
    run(&x, 1800);
    CHECK("overshoot above the lowest level stays DEMAND", x.out.state == HEATCTL_DEMAND && x.out.demand > 40);
    x.in.room = 26; // Far over: the PI takes demand to the lowest level, then heating goes off
    int t = run_until(&x, HEATCTL_IDLE, -1, 3600);
    CHECK("far over: IDLE once at the lowest level", t > 0 && t <= 1200);
    CHECK("far over: IDLE reason", strstr(x.out.reason, "room") != NULL);
    start(&x); // Mild: already at the lowest level
    x.in.room = 21.6;
    run(&x, 1799);
    CHECK("mild: at the lowest level before IDLE", x.out.state == HEATCTL_DEMAND && x.out.demand == 40);
    run(&x, 1);
    CHECK("mild: IDLE from the lowest level", x.out.state == HEATCTL_IDLE);
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
    CHECK("IDLE: lowest level with QO", x.out.demand == 40 && x.out.quiet);

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
    CHECK("after IDLE: integral at the heat of the lowest level", NEAR(x.out.integral, 810));
    CHECK("after IDLE: setpoint demand.set", NEAR(x.out.setpoint, 30));

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
    CHECK("after COLD: integral at 95% level", NEAR(x.out.integral, 1920));

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
    x.in.liquid = -20;
    run(&x, 1);
    CHECK("defrost entered", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_DEFROST && x.out.changed);
    float integral = x.out.integral;
    run(&x, 1000);
    CHECK("defrost holds demand", x.out.demand == 60);
    CHECK("defrost QO on", x.out.quiet);
    CHECK("defrost holds setpoint", NEAR(x.out.setpoint, 30));
    CHECK("defrost freezes integral", x.out.integral == integral);
    x.in.liquid = 26;
    run(&x, 1);
    CHECK("defrost exit on liquid", x.out.sub == HEATCTL_SUB_HOLD && x.out.changed && x.out.resend_fan);
    run(&x, 1);
    CHECK("fan resend only once", !x.out.resend_fan);
    integral = x.out.integral;
    run(&x, 598);
    CHECK("hold: integral keeps running", x.out.integral > integral);
    CHECK("hold: demand not raised", x.out.demand == 60);
    CHECK("hold: still in hold", x.out.sub == HEATCTL_SUB_HOLD);
    run(&x, 1);
    CHECK("hold ends after defrost.hold", x.out.sub == HEATCTL_SUB_NONE && x.out.changed);
    run(&x, 1);
    CHECK("demand rises after hold", x.out.demand > 60);

    // Cap
    start(&x);
    x.in.liquid = -20;
    run(&x, 1);
    int t = run_until(&x, HEATCTL_DEMAND, HEATCTL_SUB_HOLD, 4000);
    CHECK("defrost capped at defrost.max", t == 1800);
    CHECK("cap reason", strstr(x.out.reason, "cap") != NULL);
    run(&x, 3600);
    CHECK("no new defrost until liquid has been hot again", x.out.sub != HEATCTL_SUB_DEFROST);

    // Liquid low from the start (unit not heating yet): not a defrost
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.liquid = -20;
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
    x.in.liquid = -20;
    x.in.room = 22.5;
    run(&x, 1799);
    CHECK("no IDLE during defrost", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_DEFROST);
    x.in.liquid = 30;
    run(&x, 1);
    CHECK("defrost over: demand to the lowest level first", x.out.state == HEATCTL_DEMAND && x.out.demand == 40);
    run(&x, 1);
    CHECK("IDLE once defrost ends", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_NONE);

    // No COLD during defrost, and outdoor frozen during defrost and the hold after it
    start(&x);
    run(&x, 1);
    x.in.liquid = -20;
    float outdoor = x.out.outdoor;
    x.in.outdoor = -25; // The unit's reading jumps during defrost
    run(&x, 1700);
    CHECK("outdoor frozen during defrost", x.out.outdoor == outdoor);
    CHECK("no COLD during defrost", x.out.state == HEATCTL_DEMAND);
    x.in.liquid = 35; // And for a while after it
    run(&x, 599);
    CHECK("outdoor frozen in the hold after a defrost", x.out.sub == HEATCTL_SUB_HOLD && x.out.outdoor == outdoor);
    run(&x, 60);
    CHECK("outdoor follows again after the hold", x.out.sub == HEATCTL_SUB_NONE && x.out.outdoor < outdoor);

    // COLD defrost: demand 100 QO off held
    memset(&x.s, 0, sizeof(x.s));
    x.in = inputs();
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    x.in.liquid = -20;
    run(&x, 1);
    CHECK("COLD defrost", x.out.state == HEATCTL_COLD && x.out.sub == HEATCTL_SUB_DEFROST);
    CHECK("COLD defrost: demand 100 QO off", x.out.demand == 100 && !x.out.quiet);
}

// The coil cooled while the controller was not heating: back in DEMAND that is not a defrost until heating is
// established again
static void test_defrost_rearm(void)
{
    sim_t x;
    start(&x);
    x.in.room = 21.6;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    x.in.liquid = x.p.defrost_enter - 1;
    x.in.room = 20.9;
    run_until(&x, HEATCTL_DEMAND, -1, 4000);
    x.changes = 0;
    run(&x, 600);
    CHECK("IDLE -> DEMAND with a cool coil is not a defrost", x.out.sub == HEATCTL_SUB_NONE && x.changes == 0);
    x.in.liquid = 35;
    run(&x, 60);
    x.in.liquid = -20;
    run(&x, 1);
    CHECK("after IDLE: defrost once heating again", x.out.sub == HEATCTL_SUB_DEFROST);

    start(&x);
    x.in.room = NAN;
    run_until(&x, HEATCTL_NOSENSOR, -1, 4000);
    x.in.liquid = x.p.defrost_enter - 1;
    run(&x, 60);
    x.in.room = 21;
    run(&x, 1);
    x.changes = 0;
    run(&x, 600);
    CHECK("NOSENSOR -> DEMAND with a cool coil is not a defrost",
          x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_NONE && x.changes == 0);

    start(&x);
    x.p.defrost_watchdog = 1;
    for (int i = 0; i < 4; i++) {
        run(&x, 600);
        x.in.liquid = -20;
        run(&x, 300);
        x.in.liquid = 35;
        run(&x, 1);
    }
    x.in.liquid = x.p.defrost_enter - 1; // Cooled during the rest
    run_until(&x, HEATCTL_DEMAND, -1, 4000);
    x.changes = 0;
    run(&x, 600);
    CHECK("rest -> DEMAND with a cool coil is not a defrost", x.out.sub == HEATCTL_SUB_NONE && x.changes == 0);

    // The unit thermostats off by itself (in COLD it does) and its coil drifts towards room temperature
    start(&x);
    run(&x, 100);
    x.in.liquid = 15;
    run(&x, 600);
    CHECK("coil at room temperature is not a defrost", x.out.sub == HEATCTL_SUB_NONE);
}

// The post-defrost hold carries over into COLD without starting again
static void test_hold_carry(void)
{
    sim_t x;
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run(&x, 100);
    x.in.liquid = -20;
    run(&x, 300);
    x.in.liquid = 35;
    run(&x, 1);
    CHECK("hold carry: hold in DEMAND", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_HOLD);
    uint32_t hold = x.now;
    run_until(&x, HEATCTL_COLD, -1, 4000);
    CHECK("hold carry: hold carried into COLD", x.out.sub == HEATCTL_SUB_HOLD);
    run_until(&x, HEATCTL_COLD, HEATCTL_SUB_NONE, 4000);
    CHECK("hold carry: hold lasts defrost.hold in all", x.now - hold == 600);
}

// In the post-defrost hold the COLD integral keeps running, but the setpoint does not rise
static void test_cold_hold(void)
{
    sim_t x;
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    run(&x, 1800); // Room at target, so the setpoint stays at cold.set past cold.rate
    x.in.liquid = -20;
    run(&x, 300);
    x.in.liquid = 35;
    run(&x, 1);
    CHECK("COLD hold: in hold", x.out.state == HEATCTL_COLD && x.out.sub == HEATCTL_SUB_HOLD);
    x.in.room = 19;
    float integral = x.out.integral, sp = x.out.setpoint;
    run(&x, 598);
    CHECK("COLD hold: integral keeps running", x.out.integral > integral);
    CHECK("COLD hold: setpoint not raised", x.out.setpoint == sp);
    run_until(&x, HEATCTL_COLD, HEATCTL_SUB_NONE, 10);
    run(&x, 1);
    CHECK("COLD hold over: setpoint rises", x.out.setpoint > sp);
}

// defrost.hold 0 turns the hold off
static void test_no_hold(void)
{
    sim_t x;
    start(&x);
    x.p.defrost_hold = 0;
    x.in.room = 18;
    run(&x, 5 * 3600);
    x.in.liquid = -20;
    run(&x, 1);
    x.in.liquid = 30;
    run(&x, 1);
    CHECK("no hold: defrost ends straight to DEMAND", x.out.sub == HEATCTL_SUB_NONE && x.out.resend_fan);
    CHECK("no hold: demand not capped", x.out.demand == 100);
    run(&x, 600);
    CHECK("no hold: demand stays at the top", x.out.demand == 100);
}

// demand.table saved with fewer levels during a defrost
static void test_table_shrinks_in_defrost(void)
{
    sim_t x;
    start(&x);
    x.in.room = 18;
    run(&x, 5 * 3600);
    x.in.liquid = -20;
    run(&x, 1);
    CHECK("table shrink: defrost at plain 100", x.out.sub == HEATCTL_SUB_DEFROST && x.out.demand == 100);
    CHECK("table shrink: parsed", !heatctl_parse_table(&x.p.table, "40:220,60:400,80:700", DEMAND_TABLE));
    run(&x, 1);
    CHECK("table shrink: demand from the new table", x.out.demand == 80);
}

// auto.smooth 0 with two steps in one second (CN_WIRED has no 1 s sleep)
static void test_no_smoothing(void)
{
    sim_t x;
    start(&x);
    x.p.smooth = 0;
    x.in.room = 20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    CHECK("no smoothing: room is the reading", x.out.room == 20);
    run(&x, 600);
    CHECK("no smoothing: integral stays a number", !isnan(x.out.integral) && x.out.integral > 350);
}

// max is defrost.holdmax: 100 is 100q, never plain 100
static void test_hold_cap(int full, int max)
{
    sim_t x;
    start(&x);
    x.p.demand_full = full;
    x.p.defrost_holdmax = max;
    x.in.room = 18;
    run(&x, 5 * 3600);
    if (full)
        CHECK("cold room reaches plain 100", x.out.demand == 100 && !x.out.quiet);
    else
        CHECK("demand.full off tops out at 100q", x.out.demand == 100 && x.out.quiet);
    CHECK("anti-windup: integral at most top level", x.out.integral <= (full ? 7410 : 4530) + 1e-3f);
    x.in.liquid = -20;
    run(&x, 1);
    CHECK("defrost at 100: QO on", x.out.sub == HEATCTL_SUB_DEFROST && x.out.demand == 100 && x.out.quiet);
    x.in.liquid = 30;
    run(&x, 1);
    CHECK("hold caps demand at defrost.holdmax, QO on",
          x.out.sub == HEATCTL_SUB_HOLD && x.out.demand == max && x.out.quiet);
    run(&x, 598);
    CHECK("hold keeps cap", x.out.demand == max && x.out.quiet);
    run(&x, 2);
    run(&x, 600);
    CHECK("after hold back to top", x.out.demand == 100 && x.out.quiet == !full);
}

static void test_watchdog(int defrosts)
{
    sim_t x;
    start(&x);
    x.p.defrost_watchdog = 1;
    x.p.demand_fan = 3;
    x.p.idle_fan = 6;
    for (int i = 0; i < defrosts; i++) {
        run(&x, 600);
        x.in.liquid = -20; // 5 min defrost
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
    CHECK("rest: fan auto, whatever idle.fan is", x.out.fan == 0 && x.out.resend_fan);
    run(&x, 899);
    CHECK("rest: fan auto to the end", x.out.sub == HEATCTL_SUB_REST && x.out.fan == 0);
    int t = 899 + run_until(&x, HEATCTL_DEMAND, -1, 4000);
    CHECK("rest lasts defrost.rest", t == 900);
    CHECK("after rest: the fan of the state", x.out.fan == 3);
    CHECK("after rest: integral at lowest level", NEAR(x.out.integral, 810));
}

// defrost.watchdog off (the default): no rest however many defrosts
static void test_watchdog_off(void)
{
    sim_t x;
    start(&x);
    for (int i = 0; i < 4; i++) { // As test_watchdog(4), which rests
        run(&x, 600);
        x.in.liquid = -20;
        run(&x, 300);
        x.in.liquid = 35;
        run(&x, 1);
    }
    CHECK("watchdog off: no rest", x.out.state == HEATCTL_DEMAND && x.out.sub == HEATCTL_SUB_HOLD);
}

static void test_watchdog_slow(void)
{
    sim_t x;
    start(&x);
    x.p.defrost_watchdog = 1;
    for (int i = 0; i < 6; i++) { // A defrost every 40 min: never more than 3 in 90 min
        run(&x, 2100);
        x.in.liquid = -20;
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
    x.in.room = 19; // Heat request would rise
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
    CHECK("reading back: fresh integral", NEAR(x.out.integral, 1200));

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

static void test_gap(void)
{ // The main loop stalls (comms restart): the next step must not integrate the whole gap
    sim_t x;
    start(&x);
    x.in.room = 19;
    x.now += 3600;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    CHECK("long gap: integral grows by at most a minute", x.out.integral - 1200 <= 900 * 2 * 60 / 5400.0 + 0.01);
}

static void test_rate_and_levels(void)
{
    sim_t x;
    start(&x);
    x.in.room = 19;
    run(&x, 599);
    CHECK("demand held for demand.rate", x.out.demand == 60);
    run(&x, 1);
    CHECK("one change may jump several levels", x.out.demand == 95);
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
    CHECK("warm room: lowest level", x.out.demand == 40 && x.out.state == HEATCTL_DEMAND);
    CHECK("anti-windup: integral not below lowest level", x.out.integral >= 810 - 1e-3f);
    x.in.room = 21;
    run(&x, 1200);
    CHECK("back at target: demand leaves the lowest level", x.out.demand > 40);

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

static void test_fanmap(void)
{
    heatctl_fanmap_t m;
    CHECK("fanmap ok", !heatctl_parse_fanmap(&m, "-5:5, 0:4 ,5:3,10:Q,15:a") && m.n == 5);
    CHECK("fanmap below the first point", heatctl_fanmap_fan(&m, -20) == 5);
    CHECK("fanmap at a point", heatctl_fanmap_fan(&m, 0) == 4);
    CHECK("fanmap between points", heatctl_fanmap_fan(&m, 4.9) == 4);
    CHECK("fanmap quiet", heatctl_fanmap_fan(&m, 12) == 6);
    CHECK("fanmap auto, above the last point", heatctl_fanmap_fan(&m, 30) == 0);
    CHECK("fanmap unknown outdoor", heatctl_fanmap_fan(&m, NAN) == HEATCTL_FAN_LEAVE);
    CHECK("fanmap empty", !heatctl_parse_fanmap(&m, "") && m.n == 0 && heatctl_fanmap_fan(&m, 0) == HEATCTL_FAN_LEAVE);
    const char *bad[] = {"x",
                         "0:7",
                         "0:0",
                         "0:x",
                         "0:3,-5:4",
                         "0:3,0:4",
                         "0:",
                         "0:3,",
                         "0 3",
                         "0:3;5:2",
                         "1:1,2:1,3:1,4:1,5:1,6:1,7:1,8:1,9:1"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char name[80];
        snprintf(name, sizeof(name), "fanmap bad \"%s\"", bad[i]);
        CHECK(name, heatctl_parse_fanmap(&m, bad[i]) && m.n == 0);
    }
}

// The demand.fanmap default: fan 4 below -6, 3 up to 1, 2 up to 7, then 1
static void test_fanmap_default(void)
{
    heatctl_fanmap_t m;
    CHECK("demand.fanmap default parses", !heatctl_parse_fanmap(&m, DEMAND_FANMAP) && m.n);
    const float at[][2] = {{-25, 4}, {-6.1, 4}, {-6, 3}, {0.9, 3}, {1, 2}, {6.9, 2}, {7, 1}, {15, 1}};
    for (unsigned i = 0; i < sizeof(at) / sizeof(*at); i++) {
        char name[80];
        snprintf(name, sizeof(name), "demand.fanmap default at %.1f", at[i][0]);
        CHECK(name, heatctl_fanmap_fan(&m, at[i][0]) == at[i][1]);
    }
}

// Fan by outdoor temperature, each fan kept for fan.hold at least
static void test_fan_by_outdoor(void)
{
    sim_t x;
    start(&x);
    x.p.demand_fan = HEATCTL_FAN_MAP;
    x.p.idle_fan = 6;
    x.p.fan_hold = 1800;
    heatctl_parse_fanmap(&x.p.demand_fanmap, "-5:5,0:4,5:3");
    run(&x, 1);
    CHECK("fan by outdoor: set at once", x.out.fan == 3);
    x.in.outdoor = 2;
    run(&x, 1798);
    CHECK("fan by outdoor: held", x.out.fan == 3 && x.out.outdoor < 5);
    run(&x, 2);
    CHECK("fan by outdoor: changes once the hold is over", x.out.fan == 4);
    x.in.outdoor = 8;
    run(&x, 1700);
    CHECK("fan by outdoor: held again, no flapping", x.out.fan == 4 && x.out.outdoor > 5);
    run(&x, 200);
    CHECK("fan by outdoor: and back", x.out.fan == 3);

    x.in.liquid = 0; // A defrost is no time for a fan change
    x.in.outdoor = 2;
    run(&x, 1);
    CHECK("fan by outdoor: defrost", x.out.sub == HEATCTL_SUB_DEFROST);
    x.s.outdoor = 2;
    run(&x, 1750);
    CHECK("fan by outdoor: no change in a defrost", x.out.sub == HEATCTL_SUB_DEFROST && x.out.fan == 3);
    x.in.liquid = 35;
    run(&x, 1);
    CHECK("fan by outdoor: resent after the defrost", x.out.sub == HEATCTL_SUB_HOLD && x.out.resend_fan);
    run(&x, 598);
    CHECK("fan by outdoor: no change in the hold after a defrost", x.out.sub == HEATCTL_SUB_HOLD && x.out.fan == 3);
    run(&x, 2);
    CHECK("fan by outdoor: changes after the hold", x.out.sub == HEATCTL_SUB_NONE && x.out.fan == 4);

    x.in.room = 22; // Another state has its own fan, and the hold starts afresh on the way back
    x.in.outdoor = -10;
    CHECK("fan by outdoor: IDLE", run_until(&x, HEATCTL_IDLE, -1, 3 * 3600) > 0 && x.out.fan == 6);
    run(&x, 3 * 3600);
    x.in.room = 20;
    CHECK("fan by outdoor: at once on entering the state",
          run_until(&x, HEATCTL_DEMAND, -1, 900) > 0 && x.out.fan == 5);

    x.p.demand_fan = 2; // A change of setting applies at once
    run(&x, 1);
    CHECK("fan by outdoor: to fixed", x.out.fan == 2);
    x.p.demand_fan = HEATCTL_FAN_MAP;
    run(&x, 1);
    CHECK("fan by outdoor: and back to the map", x.out.fan == 5);

    start(&x); // The hold is within a state only: each state's fan is set at once on entering it
    x.p.demand_fan = x.p.idle_fan = x.p.cold_fan = HEATCTL_FAN_MAP;
    x.p.fan_hold = 24 * 3600;
    heatctl_parse_fanmap(&x.p.demand_fanmap, "0:3");
    heatctl_parse_fanmap(&x.p.idle_fanmap, "0:Q");
    heatctl_parse_fanmap(&x.p.cold_fanmap, "0:5");
    run(&x, 1);
    CHECK("fan hold: DEMAND", x.out.fan == 3);
    x.in.room = 22;
    CHECK("fan hold: IDLE at once", run_until(&x, HEATCTL_IDLE, -1, 3600) > 0 && x.out.fan == 6);
    x.in.room = 20;
    CHECK("fan hold: DEMAND at once", run_until(&x, HEATCTL_DEMAND, -1, 3600) > 0 && x.out.fan == 3);
    x.in.outdoor = -20;
    CHECK("fan hold: COLD at once", run_until(&x, HEATCTL_COLD, -1, 3 * 3600) > 0 && x.out.fan == 5);
    x.in.outdoor = 5;
    CHECK("fan hold: DEMAND at once from COLD", run_until(&x, HEATCTL_DEMAND, -1, 3 * 3600) > 0 && x.out.fan == 3);

    heatctl_parse_fanmap(&x.p.demand_fanmap, "-5:5,0:4,5:3"); // Also by way of a state with no fan of its own
    run(&x, 1);
    CHECK("fan hold: held in DEMAND", x.out.fan == 3);
    x.in.room = NAN;
    x.in.outdoor = -10;
    CHECK("fan hold: NOSENSOR", run_until(&x, HEATCTL_NOSENSOR, -1, 3600) > 0 && x.out.fan == HEATCTL_FAN_LEAVE);
    run(&x, 3 * 3600);
    x.in.room = 21;
    CHECK("fan hold: DEMAND at once from NOSENSOR", run_until(&x, HEATCTL_DEMAND, -1, 60) > 0 && x.out.fan == 5);

    start(&x); // No outdoor temperature: left alone until there is one
    x.p.demand_fan = HEATCTL_FAN_MAP;
    x.p.fan_hold = 1800;
    heatctl_parse_fanmap(&x.p.demand_fanmap, "-5:5,0:4,5:3");
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = NAN;
    run(&x, 10);
    CHECK("fan by outdoor: unknown outdoor leaves the fan", x.out.fan == HEATCTL_FAN_LEAVE);
    x.in.outdoor = 1;
    run(&x, 1);
    CHECK("fan by outdoor: set once outdoor is known", x.out.fan == 4);

    x.p.demand_fanmap.n = 0; // No map: left alone
    start(&x);
    x.p.demand_fan = HEATCTL_FAN_MAP;
    run(&x, 1);
    CHECK("fan by outdoor: no map leaves the fan", x.out.fan == HEATCTL_FAN_LEAVE);
}

// Electrical watts the unit draws for the controller's outputs
static float model_watts(const sim_t *x)
{
    if (x->out.state != HEATCTL_DEMAND || x->out.sub == HEATCTL_SUB_DEFROST)
        return 0; // IDLE: setpoint far below the room, so the unit stops
    for (int r = x->p.table.n - 1; r >= 0; r--)
        if (x->p.table.level[r].demand == x->out.demand &&
            (x->p.table.level[r].quiet == x->out.quiet || x->out.demand < 100))
            return x->p.table.level[r].watts;
    return 0;
}

// Room model: first-order (one node, capacity C, loss UA x (room - outdoor), COP 3), or the two-node house from the
// heat-model PoC (room air 2 MJ/K, building mass 40 MJ/K, COP 3.3 + 0.07 x outdoor, 10 min heat pump lag, BLE in 0.1
// steps). Checks the loop settles without hunting between DEMAND and IDLE. two_node 2 is that house with the heat
// output known to the controller and a 9 min defrost every hour, which alone swings that room by a degree.
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
        if (two_node == 2) {
            x.in.liquid = (i % 3600 >= 3600 - 540) ? -20 : 35;
            if (x.in.liquid < 0)
                w = q = 0;
            x.in.heatout = q;
        }
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
        snprintf(name, sizeof(name), "two-node house%s outdoor %.0f C: %d IDLE, room %.2f..%.2f",
                 two_node == 2 ? " with defrosts" : "", outdoor, idles, lo, hi);
    else
        snprintf(name, sizeof(name), "first-order room outdoor %.0f C %.0f MJ/K: %d IDLE, room %.2f..%.2f", outdoor,
                 capacity / 1e6, idles, lo, hi);
    CHECK(name, idles == 0 && lo > (two_node == 2 ? 20 : 20.5f) && hi < (two_node == 2 ? 22.3f : 21.5f));
    printf("%s\n", name);
}

// Heat (W) the unit gives for the controller's outputs. Not the table times demand.cop: the controller is to go by
// the heat it is told of
static float model_heat(const sim_t *x)
{
    if (x->out.state != HEATCTL_DEMAND)
        return 0;
    if (x->out.demand == 100)
        return x->out.quiet ? 3900 : 6800;
    return model_watts(x) * 3.4f;
}

typedef struct {
    float got, request; // Mean heat given and requested (W)
    int changes;        // Demand changes
    uint32_t shortest;  // Shortest time between two of them (s)
    uint32_t full;      // Time at plain 100 (s)
    uint8_t lo, hi;     // Lowest and highest demand
} account_t;

// Run for secs. The heat the unit gives is fed back (known), or it is not and the unit gives the table's watts times
// demand.cop. A defrost of outage seconds ends every cadence seconds (0 for none).
static account_t run_heat(sim_t *x, uint32_t secs, int known, uint32_t cadence, uint32_t outage)
{
    account_t a = {.lo = 255, .shortest = UINT32_MAX};
    double got = 0, request = 0;
    int level = x->s.level;
    uint32_t at = 0;
    for (uint32_t i = 0; i < secs; i++) {
        uint8_t defrost = (cadence && i % cadence >= cadence - outage);
        float heat = defrost ? 0 : known ? model_heat(x) : model_watts(x) * x->p.cop;
        x->in.liquid = defrost ? -20 : 35;
        x->in.heatout = known ? heat : NAN;
        run(x, 1);
        got += heat;
        request += x->out.heatw;
        if (x->s.level != level) {
            if (at && x->now - at < a.shortest)
                a.shortest = x->now - at;
            at = x->now;
            level = x->s.level;
            a.changes++;
        }
        if (x->out.demand < a.lo)
            a.lo = x->out.demand;
        if (x->out.demand > a.hi)
            a.hi = x->out.demand;
        if (x->out.demand == 100 && !x->p.table.level[x->s.level].quiet)
            a.full++;
    }
    a.got = got / secs;
    a.request = request / secs;
    return a;
}

// Heat request held still: P only, on a room under the target
static void start_heat(sim_t *x, float room)
{
    start(x);
    x->p.ti = 0;
    x->p.smooth = 0;
    x->in.room = room;
}

// Heat deficit: demand steps between the levels around the heat request, so the heat given is the heat request
static void test_heat_deficit(void)
{
    sim_t x;
    start_heat(&x, 19);
    run_heat(&x, 3 * 3600, 1, 0, 0);
    account_t a = run_heat(&x, 48 * 3600, 1, 0, 0);
    printf("heat deficit: request %.0f W, given %.0f W, demand %d..%d, %d changes\n", a.request, a.got, a.lo, a.hi,
           a.changes);
    CHECK("heat deficit: request lies between the heat of 95 and of 100q", a.request > 2300 && a.request < 3800);
    CHECK("heat deficit: the heat given is the heat request", fabsf(a.got - a.request) < 0.03f * a.request);
    CHECK("heat deficit: from the two levels around it", a.lo == 95 && a.hi == 100 && !a.full);
    CHECK("heat deficit: demand changes at most once per demand.rate", a.changes > 2 && a.shortest >= 600);
    // 3.4 W of heat per W of the table at 95, 2.6 at 100q
    CHECK("heat deficit: heat per watt of the table learnt from the unit", x.out.scale > 2.6 && x.out.scale < 3.4);
    CHECK("heat deficit: no defrost, so the heat target is the heat request", NEAR(x.out.heattarget, x.out.heatw));

    // The heat output is not known: the table times demand.cop stands in for it
    start_heat(&x, 19);
    run_heat(&x, 3 * 3600, 0, 0, 0);
    a = run_heat(&x, 48 * 3600, 0, 0, 0);
    printf("heat deficit, heat not known: request %.0f W, given %.0f W, demand %d..%d\n", a.request, a.got, a.lo, a.hi);
    CHECK("heat not known: the table's heat is the heat request", fabsf(a.got - a.request) < 0.03f * a.request);
    CHECK("heat not known: from the two levels around it", a.lo == 95 && a.hi == 100 && !a.full);
    CHECK("heat not known: heat per watt of the table is demand.cop", NEAR(x.out.scale, 3));

    // A unit that gives more or less heat than demand.cop says is no matter
    for (int i = 0; i < 2; i++) {
        start_heat(&x, 19);
        x.p.cop = i ? 4.5 : 2;
        run_heat(&x, 3 * 3600, 1, 0, 0);
        a = run_heat(&x, 48 * 3600, 1, 0, 0);
        CHECK(i ? "demand.cop high: the heat given is the heat request"
                : "demand.cop low: the heat given is the heat request",
              fabsf(a.got - a.request) < 0.03f * a.request);
    }
}

// Defrosts cost heat, and the more of them the more: the account makes it up, here with plain 100 after a defrost.
// Not all of it, as demand only goes up from a level that itself gives too little: the rest is for the integral
static void test_heat_deficit_defrost(void)
{
    account_t a[2];
    for (int i = 0; i < 2; i++) {
        sim_t x;
        start_heat(&x, 18.6);
        uint32_t cadence = i ? 90 * 60 : 60 * 60;
        run_heat(&x, 3 * cadence, 1, cadence, 12 * 60);
        a[i] = run_heat(&x, 48 * 3600, 1, cadence, 12 * 60);
        printf(
            "heat deficit, defrost every %d min: request %.0f W, given %.0f W, demand %d..%d, plain 100 for %d min\n",
            (int)cadence / 60, a[i].request, a[i].got, a[i].lo, a[i].hi, (int)a[i].full / 60);
        CHECK(i ? "defrost every 90 min: the heat given is the heat request"
                : "defrost every 60 min: the heat given is the heat request",
              fabsf(a[i].got - a[i].request) < 0.05f * a[i].request);
        CHECK("defrosts: demand changes at most once per demand.rate", a[i].shortest >= 600);
        CHECK("defrosts: the heat target is over the heat request, to make up for them",
              x.out.heattarget > x.out.heatw + 100 && x.out.heattarget < x.out.heatw + 1500);
    }
    CHECK("more defrosts take more plain 100", a[0].full > a[1].full + 3600);
}

// A big change of the heat request takes demand several levels at once, as far as the heat the unit gives now says
static void test_heat_deficit_jump(void)
{
    sim_t x;
    start_heat(&x, 21);
    for (int i = 0; i < 2 * 3600; i++) { // Mild weather, 5 W of heat per W: even the lowest level gives too much
        x.in.heatout = model_watts(&x) * 5;
        run(&x, 1);
    }
    CHECK("jump: at the lowest level", x.out.demand == 40);
    x.in.room = 19.9; // 2190 W requested: 60% gives 2000 W and 70% 2325 W (by demand.cop, 95% would give 1920 W)
    for (int i = 0; i < 3600 && x.out.demand == 40; i++) {
        x.in.heatout = model_watts(&x) * 5;
        run(&x, 1);
    }
    CHECK("jump up: to the level under the heat request", x.out.demand == 60);
    x.in.room = 23; // Far too warm: nothing requested
    for (int i = 0; i < 1800 && x.out.demand == 60; i++) {
        x.in.heatout = model_watts(&x) * 5;
        run(&x, 1);
    }
    CHECK("jump down: to the lowest level", x.out.demand == 40);
}

// The top level cannot give the heat request: the integral stops, no deficit builds, and demand comes down soon after
// the room is over the target
static void test_heat_deficit_top(void)
{
    sim_t x;
    start(&x);
    x.p.demand_full = 0;
    x.in.room = 20;
    run_heat(&x, 8 * 3600, 1, 0, 0);
    float i8 = x.out.integral;
    CHECK("top: at the top level", x.out.demand == 100 && x.out.quiet);
    run_heat(&x, 8 * 3600, 1, 0, 0);
    // It stops a little over, as it ran on while the heat per watt of the table was still being learnt
    CHECK("top: the integral stops where the heat request is what the top level gives",
          fabsf(x.out.integral - i8) < 20 && fabsf(x.out.heatw - 3900) < 250);
    CHECK("top: no heat deficit that the top level cannot make up", x.out.deficit <= 0.01);
    x.in.room = 21.5;
    int t = 0;
    while (t < 6 * 3600 && x.out.demand == 100) {
        x.in.heatout = model_heat(&x);
        run(&x, 1);
        t++;
    }
    CHECK("top: demand comes down once the room is over the target", t < 1200);
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

static void test_no_integral(void)
{ // ti 0: no integral, it stays where it is, so P only around that
    sim_t x;
    start(&x);
    x.p.ti = 0;
    run(&x, 60);
    CHECK("ti 0: DEMAND", x.out.state == HEATCTL_DEMAND);
    float i0 = x.out.integral;
    x.in.room = 20.5;
    run(&x, 2 * 3600);
    CHECK("ti 0: integral does not move", NEAR(x.out.integral, i0));
    CHECK("ti 0: heat request is kp x error on top",
          fabsf(x.out.heatw - (i0 + 900 * 0.5f)) < 1.5f); // Smoothing leaves a hair of error
    x.p.ti = 90 * 60;                                     // Back on: it moves again
    run(&x, 600);
    CHECK("ti back on: integral moves", x.out.integral > i0 + 1);

    // COLD: cold.ti 0 keeps the integral at cold.set
    memset(&x.s, 0, sizeof(x.s));
    x.p.cold_ti = 0;
    x.in.room = 21;
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    x.in.room = 20;
    run(&x, 6 * 3600);
    CHECK("cold.ti 0: integral stays at cold.set", NEAR(x.out.integral, 23));
    CHECK("cold.ti 0: setpoint is cold.set + cold.kp x error", NEAR(x.out.setpoint, 25));
}

// The fan.flow default: air flow (m3/h) at any fan speed the unit may report, not only at the fan settings
static void test_fan_flow(void)
{
    heatctl_curve_t c;
    CHECK("fan.flow default parses", !heatctl_parse_curve(&c, FAN_FLOW) && c.n);
    const float at[][2] = {{510, 342}, {570, 426}, {660, 534}, {750, 636}, {940, 828}, {1120, 1104}};
    for (unsigned i = 0; i < sizeof(at) / sizeof(*at); i++) {
        char name[60];
        snprintf(name, sizeof(name), "fan.flow at %.0f rpm", at[i][0]);
        CHECK(name, NEAR(heatctl_curve_offset(&c, at[i][0]), at[i][1]));
    }
    float f = heatctl_curve_offset(&c, 700);
    CHECK("fan.flow between two settings", f > 534 && f < 636);
    f = heatctl_curve_offset(&c, 300); // Slowed for a cool coil, below the quiet setting
    CHECK("fan.flow below the slowest setting", f > 150 && f < 342);
    CHECK("fan.flow stopped", NEAR(heatctl_curve_offset(&c, 0), 0));
    CHECK("fan.flow above the fastest setting", heatctl_curve_offset(&c, 1300) > 1104);
}

// State kept over a restart (heatctl_keep, heatctl_resume)

#define CLOCK 100000 // Clock that runs on over a restart (s), when the block is written

// CRC-32 (as zlib's), to seal a block the tests have changed
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *b = data;
    uint32_t c = 0xFFFFFFFF;
    while (len--) {
        c ^= *b++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

static void seal(heatctl_keep_t *k)
{
    k->crc = crc32(k, offsetof(heatctl_keep_t, crc));
}

// The block as Faikout.c writes it after a step
static heatctl_keep_t keep(const sim_t *x)
{
    heatctl_keep_t k;
    memset(&k, 0x55, sizeof(k)); // RTC memory holds whatever it held
    heatctl_keep(&x->s, &x->p, &k, CLOCK);
    return k;
}

// A restart: a new controller, uptime from 1, given the kept block at clock. The step is left to the caller.
static void reboot(sim_t *x, const heatctl_keep_t *k, uint32_t clock)
{
    memset(&x->s, 0, sizeof(x->s));
    x->now = 1;
    heatctl_resume(&x->s, k, clock, x->now);
}

// A fresh start, as without a kept block
static int fresh(const sim_t *x)
{
    return x->out.state == HEATCTL_DEMAND && x->out.changed && x->out.demand == 60 && NEAR(x->out.integral, 1200) &&
           !x->out.resumed && !strncmp(x->out.reason, "start", 5);
}

// DEMAND: cold room at a high level, learnt scale, a defrost not yet made up
static void demand_moved(sim_t *x)
{
    start(x);
    x->in.room = 19.5;
    x->in.heatout = 1500;
    run(x, 3600);
    x->in.liquid = -20;
    run(x, 300);
    x->in.liquid = 35;
    run(x, 60);
    x->in = inputs();
}

static void test_keep_demand(void)
{
    sim_t x;
    demand_moved(&x);
    heatctl_t was = x.s;
    heatctl_out_t o = x.out;
    CHECK("keep demand: moved from the start", o.state == HEATCTL_DEMAND && o.demand != 60 && !NEAR(o.integral, 1200) &&
                                                   x.s.debt > 1 && fabsf(x.s.deficit) > 1 && !NEAR(x.s.scale, 3));
    heatctl_keep_t k = keep(&x);
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("resume demand: DEMAND", x.out.state == HEATCTL_DEMAND && x.out.changed);
    CHECK("resume demand: level kept", x.out.demand == o.demand && x.out.quiet == o.quiet);
    CHECK("resume demand: integral kept", x.out.integral == was.integral);
    CHECK("resume demand: scale, deficit and debt kept",
          x.s.scale == was.scale && x.s.deficit == was.deficit && x.s.debt == was.debt);
    CHECK("resume demand: no hold kept", x.out.sub == HEATCTL_SUB_NONE);
    CHECK("resume demand: reason", strstr(x.out.reason, "resumed") && strstr(x.out.reason, "demand"));
    CHECK("resume demand: shown", x.out.resumed);
    uint8_t demand = x.out.demand;
    x.in.room = 25; // Far over: demand would come down, but not before demand.rate from the resume
    run(&x, 598);
    CHECK("resume demand: demand.rate counts from the resume", x.out.demand == demand);
    run(&x, 2);
    CHECK("resume demand: then it moves", x.out.demand < demand);
    run(&x, HEATCTL_RESUMED_SHOW);
    CHECK("resume demand: shown for a while only", !x.out.resumed);

    // Without a kept block it starts as before
    heatctl_keep_t none;
    memset(&none, 0, sizeof(none));
    x.in = inputs();
    reboot(&x, &none, CLOCK + 60);
    run(&x, 1);
    CHECK("nothing kept: fresh start", fresh(&x) && !strcmp(x.out.reason, "start"));
}

static void test_keep_cold(void)
{
    sim_t x;
    start(&x);
    memset(&x.s, 0, sizeof(x.s));
    x.in.outdoor = -20;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now);
    run_until(&x, HEATCTL_COLD, -1, 4000);
    x.in.room = 19;
    run(&x, 2 * 3600);
    heatctl_out_t o = x.out;
    float integral = x.s.cold_integral;
    CHECK("keep cold: setpoint moved", o.state == HEATCTL_COLD && o.setpoint > 23.4f);
    heatctl_keep_t k = keep(&x);
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("resume cold: COLD", x.out.state == HEATCTL_COLD && x.out.changed && x.out.resumed);
    CHECK("resume cold: setpoint and integral kept", x.out.setpoint == o.setpoint && x.out.integral == integral);
    CHECK("resume cold: demand 100", x.out.demand == 100 && !x.out.quiet);
    CHECK("resume cold: reason", strstr(x.out.reason, "resumed") && strstr(x.out.reason, "setpoint"));
    run(&x, 4 * 3600);
    CHECK("resume cold: stays COLD while cold", x.out.state == HEATCTL_COLD);

    // Warm after the restart: back to DEMAND only after cold.hold
    x.in = inputs();
    x.in.outdoor = -5;
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("resume cold, warm: COLD", x.out.state == HEATCTL_COLD);
    int t = run_until(&x, HEATCTL_DEMAND, -1, 4000);
    CHECK("resume cold, warm: DEMAND after cold.hold", t == 900);
}

static void test_keep_idle(void)
{
    sim_t x;
    start(&x);
    x.in.room = 21.6;
    run_until(&x, HEATCTL_IDLE, -1, 4000);
    run(&x, 1500);
    heatctl_keep_t k = keep(&x);
    reboot(&x, &k, CLOCK + 60);
    x.in.room = 19;
    run(&x, 1);
    CHECK("resume idle: IDLE", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_NONE && x.out.changed);
    CHECK("resume idle: setpoint idle.temp", NEAR(x.out.setpoint, 16));
    CHECK("resume idle: reason", strstr(x.out.reason, "resumed") != NULL);
    run(&x, 1799);
    CHECK("resume idle: idle.min counts from the resume", x.out.state == HEATCTL_IDLE);
    run(&x, 1);
    CHECK("resume idle: then DEMAND at the lowest level", x.out.state == HEATCTL_DEMAND && NEAR(x.out.integral, 810));
}

// The defrost-loop watchdog rest is kept, and lasts defrost.rest from the resume
static void test_keep_rest(void)
{
    sim_t x;
    start(&x);
    x.p.defrost_watchdog = 1;
    for (int i = 0; i < 4; i++) {
        run(&x, 600);
        x.in.liquid = -20;
        run(&x, 300);
        x.in.liquid = 35;
        run(&x, 1);
    }
    run(&x, 600);
    CHECK("keep rest: resting", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_REST);
    heatctl_keep_t k = keep(&x);
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("resume rest: IDLE rest", x.out.state == HEATCTL_IDLE && x.out.sub == HEATCTL_SUB_REST && x.out.fan == 0);
    run(&x, 899);
    CHECK("resume rest: defrost.rest from the resume", x.out.sub == HEATCTL_SUB_REST);
    run(&x, 1);
    CHECK("resume rest: then DEMAND at the lowest level", x.out.state == HEATCTL_DEMAND && NEAR(x.out.integral, 810));
}

// NOSENSOR is not kept: a restart starts afresh, as leaving NOSENSOR does
static void test_keep_nosensor(void)
{
    sim_t x;
    demand_moved(&x);
    x.in.room = NAN;
    run_until(&x, HEATCTL_NOSENSOR, -1, 4000);
    heatctl_keep_t k = keep(&x);
    x.in.room = 21;
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("nosensor not kept: fresh start", fresh(&x));
}

static void test_keep_off(void)
{
    const char *why[] = {"disabled", "unit off", "not heating"};
    for (int i = 0; i < 3; i++) {
        sim_t x;
        demand_moved(&x);
        if (i == 0)
            x.in.enabled = 0;
        else if (i == 1)
            x.in.power = 0;
        else
            x.in.heat = 0;
        run(&x, 1);
        heatctl_keep_t k = keep(&x);
        x.in = inputs();
        reboot(&x, &k, CLOCK + 60);
        run(&x, 1);
        char name[60];
        snprintf(name, sizeof(name), "off (%s): nothing kept", why[i]);
        CHECK(name, fresh(&x));
    }

    // At boot the unit's state is not known yet: the block is not written over until it has been tried, over a
    // second restart too
    sim_t x;
    demand_moved(&x);
    heatctl_out_t o = x.out;
    heatctl_keep_t k = keep(&x);
    x.in.power = 0;
    reboot(&x, &k, CLOCK + 60);
    for (int i = 0; i < 20; i++) {
        run(&x, 1);
        heatctl_keep(&x.s, &x.p, &k, CLOCK + 60 + x.now);
    }
    reboot(&x, &k, CLOCK + 90);
    for (int i = 0; i < 20; i++) {
        run(&x, 1);
        heatctl_keep(&x.s, &x.p, &k, CLOCK + 90 + x.now);
    }
    x.in.power = 1;
    run(&x, 1);
    CHECK("unknown at boot: resumed later", x.out.resumed && x.out.demand == o.demand && x.out.integral == o.integral);
    heatctl_keep(&x.s, &x.p, &k, CLOCK + 90 + x.now);
    x.in.enabled = 0; // Switched off after the resume: nothing kept
    run(&x, 1);
    heatctl_keep(&x.s, &x.p, &k, CLOCK + 90 + x.now);
    x.in.enabled = 1;
    reboot(&x, &k, CLOCK + 200);
    run(&x, 1);
    CHECK("off after the resume: nothing kept", fresh(&x));

    // Unit off at boot for longer than the block lasts: not resumed, and written over
    demand_moved(&x);
    k = keep(&x);
    x.in.power = 0;
    reboot(&x, &k, CLOCK + 60);
    for (int i = 0; i < HEATCTL_KEEP_AGE - 60; i++) {
        run(&x, 1);
        heatctl_keep(&x.s, &x.p, &k, CLOCK + 60 + x.now);
    }
    x.in.power = 1;
    x.out = heatctl_step(&x.s, &x.p, &x.in, x.now); // Same step: the block is exactly HEATCTL_KEEP_AGE old
    CHECK("off at boot: resumed up to HEATCTL_KEEP_AGE", x.out.resumed);
    demand_moved(&x);
    k = keep(&x);
    x.in.power = 0;
    reboot(&x, &k, CLOCK + 60);
    for (int i = 0; i < HEATCTL_KEEP_AGE - 59; i++) {
        run(&x, 1);
        heatctl_keep(&x.s, &x.p, &k, CLOCK + 60 + x.now);
    }
    x.in.power = 1;
    reboot(&x, &k, CLOCK + 60 + x.now);
    run(&x, 1);
    CHECK("off at boot past HEATCTL_KEEP_AGE: written over", fresh(&x));
    demand_moved(&x);
    k = keep(&x);
    x.in.power = 0;
    reboot(&x, &k, CLOCK + 60);
    run(&x, HEATCTL_KEEP_AGE - 59);
    x.in.power = 1;
    run(&x, 1);
    CHECK("off at boot past HEATCTL_KEEP_AGE: not resumed", fresh(&x));

    // Switched off at boot: not resumed, and written over
    demand_moved(&x);
    k = keep(&x);
    x.in.enabled = 0;
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    heatctl_keep(&x.s, &x.p, &k, CLOCK + 60 + x.now);
    x.in.enabled = 1;
    run(&x, 1);
    CHECK("disabled at boot: not resumed", fresh(&x));
    reboot(&x, &k, CLOCK + 70);
    run(&x, 1);
    CHECK("disabled at boot: written over", fresh(&x));
}

// A block that is damaged, stale or does not fit is not used: the start is as without one
static void test_keep_reject(void)
{
    sim_t x;
    demand_moved(&x);
    const heatctl_keep_t good = keep(&x);
    sim_t cold;
    start(&cold);
    memset(&cold.s, 0, sizeof(cold.s));
    cold.in.outdoor = -20;
    cold.out = heatctl_step(&cold.s, &cold.p, &cold.in, cold.now);
    run_until(&cold, HEATCTL_COLD, -1, 4000);
    const heatctl_keep_t goodcold = keep(&cold);
    struct {
        const char *name;
        int field;
        float value;
        uint32_t clock;
        int cold;
        int sealed;
    } bad[] = {
        {"bad checksum", 1, 1, CLOCK, 0, 0},
        {"wrong magic", 2, 0, CLOCK, 0, 1},
        {"wrong version", 3, 0, CLOCK, 0, 1},
        {"too old", 0, 0, CLOCK + HEATCTL_KEEP_AGE + 1, 0, 1},
        {"clock gone back", 0, 0, CLOCK - 1, 0, 1},
        {"integral NAN", 1, NAN, CLOCK, 0, 1},
        {"integral infinite", 1, INFINITY, CLOCK, 0, 1},
        {"deficit NAN", 4, NAN, CLOCK, 0, 1},
        {"debt infinite", 5, -INFINITY, CLOCK, 0, 1},
        {"scale NAN", 6, NAN, CLOCK, 0, 1},
        {"integral below the lowest level", 1, 800, CLOCK, 0, 1},
        {"integral above the top level", 1, 7500, CLOCK, 0, 1},
        {"scale out of range", 6, 7, CLOCK, 0, 1},
        {"scale negative", 6, -1, CLOCK, 0, 1},
        {"level not in the table", 7, 55, CLOCK, 0, 1},
        {"level with QO not in the table", 8, 0, CLOCK, 0, 1},
        {"state not kept", 9, HEATCTL_NOSENSOR, CLOCK, 0, 1},
        {"state unknown", 9, 7, CLOCK, 0, 1},
        {"rest to resume unknown", 10, HEATCTL_IDLE, CLOCK, 0, 1},
        {"cold setpoint above t.max", 11, 32.5, CLOCK, 1, 1},
        {"cold integral below t.min", 12, 15, CLOCK, 1, 1},
        {"cold setpoint NAN", 11, NAN, CLOCK, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        heatctl_keep_t k = bad[i].cold ? goodcold : good;
        switch (bad[i].field) {
        case 1:
            k.integral = bad[i].value;
            break;
        case 2:
            k.magic = 0x12345678;
            break;
        case 3:
            k.version++;
            break;
        case 4:
            k.deficit = bad[i].value;
            break;
        case 5:
            k.debt = bad[i].value;
            break;
        case 6:
            k.scale = bad[i].value;
            break;
        case 7:
            k.demand = bad[i].value;
            break;
        case 8: // 60 is in the table, but with QO
            k.demand = 60;
            k.quiet = 0;
            break;
        case 9:
            k.state = bad[i].value;
            break;
        case 10:
            k.state = HEATCTL_IDLE;
            k.rest = bad[i].value;
            break;
        case 11:
            k.setpoint = bad[i].value;
            break;
        case 12:
            k.cold_integral = bad[i].value;
            break;
        }
        if (bad[i].sealed)
            seal(&k);
        x.in = inputs();
        reboot(&x, &k, bad[i].clock + 60 * (bad[i].clock == CLOCK));
        run(&x, 1);
        char name[80];
        snprintf(name, sizeof(name), "reject %s: fresh start", bad[i].name);
        CHECK(name, fresh(&x));
        snprintf(name, sizeof(name), "reject %s: reason", bad[i].name);
        CHECK(name, bad[i].field == 2 ? !strcmp(x.out.reason, "start") : strstr(x.out.reason, "not resumed") != NULL);
    }

    // The same blocks, sealed but unchanged, resume: the rejects above are for the change alone
    heatctl_keep_t k = good;
    seal(&k);
    reboot(&x, &k, CLOCK + HEATCTL_KEEP_AGE - 1);
    run(&x, 1); // A second later: exactly HEATCTL_KEEP_AGE old
    CHECK("reject control: demand resumes at HEATCTL_KEEP_AGE", x.out.resumed);
    k = goodcold;
    seal(&k);
    reboot(&x, &k, CLOCK);
    run(&x, 1);
    CHECK("reject control: cold resumes", x.out.resumed && x.out.state == HEATCTL_COLD);
}

// One resume per boot, at the first start
static void test_keep_once(void)
{
    sim_t x;
    demand_moved(&x);
    heatctl_keep_t k = keep(&x);
    reboot(&x, &k, CLOCK + 60);
    run(&x, 1);
    CHECK("once: resumed", x.out.resumed);
    x.in.heat = 0;
    run(&x, 1);
    x.in.heat = 1;
    run(&x, 1);
    CHECK("once: the next start is fresh", fresh(&x));
    reboot(&x, &k, CLOCK + 60);
    x.in.room = NAN;
    run_until(&x, HEATCTL_NOSENSOR, -1, 4000);
    x.in.room = 21;
    run(&x, 1);
    CHECK("once: leaving NOSENSOR is fresh", x.out.state == HEATCTL_DEMAND && NEAR(x.out.integral, 1200));
}

int main(void)
{
    test_table();
    test_curve();
    test_off_and_start();
    test_demand_idle();
    test_idle_only_from_lowest();
    test_cold();
    test_defrost();
    test_defrost_rearm();
    test_hold_carry();
    test_cold_hold();
    test_no_hold();
    test_table_shrinks_in_defrost();
    test_no_smoothing();
    test_hold_cap(1, 100);
    test_hold_cap(1, 95);
    test_hold_cap(0, 100);
    test_hold_cap(0, 95);
    test_hold_cap(0, 80);
    test_watchdog(3);
    test_watchdog(4);
    test_watchdog_off();
    test_watchdog_slow();
    test_stale();
    test_gap();
    test_rate_and_levels();
    test_no_integral();
    // First-order room, loads of 300, 700 and 1550 W
    test_room_model(15, 5e6, 0);
    test_room_model(7, 5e6, 0);
    test_room_model(-10, 5e6, 0);
    test_room_model(7, 15e6, 0);
    test_room_model(-12, 0, 1);
    test_room_model(-5, 0, 1);
    test_room_model(0, 0, 1);
    test_room_model(10, 0, 1);
    test_room_model(-12, 0, 2);
    test_room_model(-8, 0, 2);
    test_room_model(-5, 0, 2);
    test_room_model(0, 0, 2);
    test_heat_deficit();
    test_heat_deficit_defrost();
    test_heat_deficit_jump();
    test_heat_deficit_top();
    test_ble_env();
    test_fan_flow();
    test_fanmap();
    test_fanmap_default();
    test_fan_by_outdoor();
    test_keep_demand();
    test_keep_cold();
    test_keep_idle();
    test_keep_rest();
    test_keep_nosensor();
    test_keep_off();
    test_keep_reject();
    test_keep_once();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
