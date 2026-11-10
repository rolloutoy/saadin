// Host unit test for the heating log (heatlog.c)
// Build and run with: make -C ESP test

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../heatctl.h"
#include "../heatlog.h"

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

static heatlog_in_t in_none(void)
{
    heatlog_in_t in = {
        .room = NAN,
        .outdoor = NAN,
        .heatw = NAN,
        .power = NAN,
        .target = NAN,
        .effective = NAN,
        .setpoint = NAN,
        .demand = NAN,
        .smoothed = NAN,
        .integral = NAN,
        .kp = NAN,
        .ti = NAN,
        .state = HEATCTL_OFF,
        .sub = HEATCTL_SUB_NONE,
    };
    return in;
}

static heatlog_in_t in_demand(void)
{
    heatlog_in_t in = {
        .room = 21,
        .outdoor = -5,
        .heatw = 400,
        .power = 410,
        .target = 21,
        .effective = 21.3,
        .setpoint = 30,
        .demand = 70,
        .smoothed = 21.1,
        .integral = 350,
        .kp = 300,
        .ti = 90,
        .state = HEATCTL_DEMAND,
        .sub = HEATCTL_SUB_NONE,
    };
    return in;
}

static void test_empty(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    CHECK("empty has no samples", heatlog_get(&l, out, 4) == 0);
    heatlog_in_t in = in_demand();
    for (uint32_t now = 1; now <= 60; now++)
        heatlog_add(&l, &in, now);
    CHECK("no sample before a full interval", heatlog_get(&l, out, 4) == 0);
    heatlog_init(&l, NULL, 4);
    for (uint32_t now = 1; now <= 200; now++)
        heatlog_add(&l, &in, now);
    CHECK("no buffer logs nothing", heatlog_get(&l, out, 4) == 0);
}

static void test_one(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_demand();
    for (uint32_t now = 1; now <= 60; now++) {
        in.room = now <= 30 ? 20 : 22;
        in.power = now <= 30 ? 400 : 500;
        in.target = now <= 50 ? 21 : 21.5;
        in.demand = now <= 50 ? 70 : 80;
        heatlog_add(&l, &in, now);
    }
    in.room = 99; // Starts the next sample, not in this one
    heatlog_add(&l, &in, 61);
    CHECK("one sample", heatlog_get(&l, out, 4) == 1);
    CHECK("sample time is its end", out[0].t == 61);
    CHECK("room is the mean", out[0].room == 2100);
    CHECK("power is the mean", out[0].power == 450);
    CHECK("heatw is the mean", out[0].heatw == 400);
    CHECK("outdoor negative", out[0].outdoor == -500);
    CHECK("target is the last", out[0].target == 2150);
    CHECK("demand is the last", out[0].demand == 80);
    CHECK("effective rounds", out[0].effective == 2130);
    CHECK("setpoint", out[0].setpoint == 3000);
    CHECK("state", out[0].state == HEATCTL_DEMAND && out[0].sub == HEATCTL_SUB_NONE);
}

static void test_unknown(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_none();
    for (uint32_t now = 1; now <= 61; now++) {
        in.room = now <= 20 ? 20.5 : NAN; // Mean of the known readings only
        heatlog_add(&l, &in, now);
    }
    CHECK("unknown one sample", heatlog_get(&l, out, 4) == 1);
    CHECK("partly known room", out[0].room == 2050);
    CHECK("unknown values",
          out[0].outdoor == HEATLOG_NONE && out[0].heatw == HEATLOG_NONE && out[0].power == HEATLOG_NONE &&
              out[0].target == HEATLOG_NONE && out[0].effective == HEATLOG_NONE && out[0].setpoint == HEATLOG_NONE &&
              out[0].demand == HEATLOG_NONE && out[0].smoothed == HEATLOG_NONE && out[0].integral == HEATLOG_NONE &&
              out[0].kp == HEATLOG_NONE && out[0].ti == HEATLOG_NONE);
    CHECK("off state", out[0].state == HEATCTL_OFF);
}

static void test_defrost(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_demand();
    for (uint32_t now = 1; now <= 61; now++) {
        in.sub = (now >= 10 && now < 20) ? HEATCTL_SUB_DEFROST : now >= 20 ? HEATCTL_SUB_HOLD : HEATCTL_SUB_NONE;
        heatlog_add(&l, &in, now);
    }
    in.sub = HEATCTL_SUB_HOLD;
    for (uint32_t now = 62; now <= 121; now++)
        heatlog_add(&l, &in, now);
    CHECK("defrost two samples", heatlog_get(&l, out, 4) == 2);
    CHECK("a defrost in the sample wins", out[0].sub == HEATCTL_SUB_DEFROST);
    CHECK("defrost does not carry over", out[1].sub == HEATCTL_SUB_HOLD);
}

static void test_tuning(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_demand();
    uint32_t now = 1;
    for (int i = 0; i < HEATLOG_INTERVAL; i++, now++) {
        in.smoothed = i < 30 ? 21 : 21.2;
        in.integral = i < 50 ? 300 : 360.4;
        in.kp = i < 50 ? 300 : 400;
        heatlog_add(&l, &in, now);
    }
    in.state = HEATCTL_COLD; // The COLD loop: integral as setpoint (°C), kp in °C per °C
    in.integral = 24.25;
    in.kp = 2;
    in.ti = 120;
    for (int i = 0; i < HEATLOG_INTERVAL; i++, now++)
        heatlog_add(&l, &in, now);
    heatlog_add(&l, &in, now);
    CHECK("tuning two samples", heatlog_get(&l, out, 4) == 2);
    CHECK("smoothed room is the mean", out[0].smoothed == 2110);
    CHECK("integral is the last, in W", out[0].integral == 360);
    CHECK("kp is the last", out[0].kp == 400);
    CHECK("ti in minutes", out[0].ti == 90);
    CHECK("cold integral in 0.01 °C", out[1].state == HEATCTL_COLD && out[1].integral == 2425);
    CHECK("cold kp in 0.01", out[1].kp == 200);
    CHECK("cold ti in minutes", out[1].ti == 120);
}

static void test_wrap(void)
{
    heatlog_sample_t buf[3], out[5];
    heatlog_t l;
    heatlog_init(&l, buf, 3);
    heatlog_in_t in = in_demand();
    uint32_t now = 1;
    for (int s = 1; s <= 5; s++)
        for (int i = 0; i < HEATLOG_INTERVAL; i++, now++) {
            in.demand = 40 + s; // Sample s has demand 40 + s
            heatlog_add(&l, &in, now);
        }
    heatlog_add(&l, &in, now); // Ends sample 5
    CHECK("wrap keeps the ring size", heatlog_get(&l, out, 5) == 3);
    CHECK("wrap oldest first", out[0].demand == 43 && out[1].demand == 44 && out[2].demand == 45);
    CHECK("wrap times", out[0].t + 60 == out[1].t && out[1].t + 60 == out[2].t && out[2].t == now);
    CHECK("max gives the newest", heatlog_get(&l, out, 2) == 2 && out[0].demand == 44 && out[1].demand == 45);
}

static void test_gap_and_clamp(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_demand();
    in.power = 40000;
    in.heatw = -40000;
    heatlog_add(&l, &in, 100);
    heatlog_add(&l, &in, 400); // A gap ends the sample
    CHECK("gap one sample", heatlog_get(&l, out, 4) == 1 && out[0].t == 400);
    CHECK("clamp high", out[0].power == INT16_MAX);
    CHECK("clamp low is not unknown", out[0].heatw == INT16_MIN + 1);
}

int main(void)
{
    test_empty();
    test_one();
    test_unknown();
    test_defrost();
    test_tuning();
    test_wrap();
    test_gap_and_clamp();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
