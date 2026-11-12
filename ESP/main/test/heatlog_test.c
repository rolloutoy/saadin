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

#define I HEATLOG_INTERVAL // The tests split a sample in thirds
_Static_assert(I % 3 == 0 && I > 10, "HEATLOG_INTERVAL");

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
        .liquid = NAN,
        .comp = NAN,
        .fan = NAN,
        .intake = NAN,
        .heatout = NAN,
        .integral = NAN,
        .owed = NAN,
        .give = NAN,
        .scale = NAN,
        .kp = NAN,
        .ti = NAN,
        .state = HEATCTL_OFF,
        .sub = HEATCTL_SUB_NONE,
        .fanset = HEATLOG_NONE8,
        .quiet = HEATLOG_NONE8,
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
        .liquid = 35,
        .comp = 40,
        .fan = 750,
        .intake = 23.5,
        .heatout = 1700,
        .integral = 350,
        .owed = 120,
        .give = 700,
        .scale = 3,
        .kp = 300,
        .ti = 90,
        .state = HEATCTL_DEMAND,
        .sub = HEATCTL_SUB_NONE,
        .fanset = 3,
        .quiet = 1,
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
    for (uint32_t now = 1; now <= I; now++)
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
    for (uint32_t now = 1; now <= I; now++) { // A third of the sample, then the rest
        uint8_t first = now <= I / 3;
        in.room = first ? 20 : 23;
        in.power = first ? 400 : 550;
        in.liquid = first ? 30 : 4.5; // A defrost dip shows in the mean
        in.comp = first ? 40 : 61;
        in.fan = first ? 750 : 0; // The fan stops in a defrost
        in.intake = first ? 23.5 : 22;
        in.heatout = first ? 1700 : -100;
        in.give = first ? 700 : 1000;
        in.target = now <= I - 5 ? 21 : 21.5;
        in.demand = now <= I - 5 ? 70 : 80;
        in.fanset = now <= I - 5 ? 3 : 5;
        in.quiet = now <= I - 5;
        heatlog_add(&l, &in, now);
    }
    in.room = 99; // Starts the next sample, not in this one
    heatlog_add(&l, &in, I + 1);
    CHECK("one sample", heatlog_get(&l, out, 4) == 1);
    CHECK("sample time is its end", out[0].t == I + 1);
    CHECK("room is the mean", out[0].room == 2200);
    CHECK("power is the mean", out[0].power == 500);
    CHECK("heatw is the mean", out[0].heatw == 400);
    CHECK("liquid is the mean", out[0].liquid == 1300);
    CHECK("comp is the mean", out[0].comp == 54);
    CHECK("fan is the mean", out[0].fan == 250);
    CHECK("intake is the mean", out[0].intake == 2250);
    CHECK("heat output is the mean", out[0].heatout == 500);
    CHECK("heat to give is the mean", out[0].give == 900);
    CHECK("outdoor negative", out[0].outdoor == -500);
    CHECK("target is the last", out[0].target == 2150);
    CHECK("demand is the last", out[0].demand == 80);
    CHECK("fan setting is the last", out[0].fanset == 5);
    CHECK("quiet outdoor is the last", out[0].quiet == 0);
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
    for (uint32_t now = 1; now <= I + 1; now++) {
        in.room = now <= 10 ? 20.5 : NAN; // Mean of the known readings only
        heatlog_add(&l, &in, now);
    }
    CHECK("unknown one sample", heatlog_get(&l, out, 4) == 1);
    CHECK("partly known room", out[0].room == 2050);
    CHECK("unknown values",
          out[0].outdoor == HEATLOG_NONE && out[0].heatw == HEATLOG_NONE && out[0].power == HEATLOG_NONE &&
              out[0].target == HEATLOG_NONE && out[0].effective == HEATLOG_NONE && out[0].setpoint == HEATLOG_NONE &&
              out[0].demand == HEATLOG_NONE && out[0].smoothed == HEATLOG_NONE && out[0].integral == HEATLOG_NONE &&
              out[0].owed == HEATLOG_NONE && out[0].give == HEATLOG_NONE && out[0].scale == HEATLOG_NONE &&
              out[0].kp == HEATLOG_NONE && out[0].ti == HEATLOG_NONE && out[0].liquid == HEATLOG_NONE &&
              out[0].comp == HEATLOG_NONE && out[0].fan == HEATLOG_NONE && out[0].intake == HEATLOG_NONE &&
              out[0].heatout == HEATLOG_NONE);
    CHECK("unknown fan setting and quiet outdoor", out[0].fanset == HEATLOG_NONE8 && out[0].quiet == HEATLOG_NONE8);
    CHECK("off state", out[0].state == HEATCTL_OFF);
}

static void test_defrost(void)
{
    heatlog_sample_t buf[4], out[4];
    heatlog_t l;
    heatlog_init(&l, buf, 4);
    heatlog_in_t in = in_demand();
    for (uint32_t now = 1; now <= I + 1; now++) {
        in.sub = (now >= 5 && now < 10) ? HEATCTL_SUB_DEFROST : now >= 10 ? HEATCTL_SUB_HOLD : HEATCTL_SUB_NONE;
        heatlog_add(&l, &in, now);
    }
    in.sub = HEATCTL_SUB_HOLD;
    for (uint32_t now = I + 2; now <= 2 * I + 1; now++)
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
        in.smoothed = i < I / 3 ? 21 : 21.3;
        in.integral = i < I - 5 ? 300 : 360.4;
        in.owed = i < I - 5 ? 100 : -42.4;
        in.scale = i < I - 5 ? 3 : 2.876;
        in.kp = i < I - 5 ? 300 : 400;
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
    CHECK("smoothed room is the mean", out[0].smoothed == 2120);
    CHECK("integral is the last, in W", out[0].integral == 360);
    CHECK("heat owed is the last, in Wh", out[0].owed == -42);
    CHECK("heat per watt of the table is the last, in 0.01", out[0].scale == 288);
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
    CHECK("wrap times", out[0].t + I == out[1].t && out[1].t + I == out[2].t && out[2].t == now);
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

// Reading by sample number, as the web page does while the log carries on
static void test_read(void)
{
    heatlog_sample_t buf[3], out[5];
    heatlog_t l;
    heatlog_init(&l, buf, 3);
    CHECK("read: empty", heatlog_begin(&l) == 0 && heatlog_end(&l) == 0 && heatlog_read(&l, 0, out, 5) == 0);
    heatlog_in_t in = in_demand();
    uint32_t now = 1;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < I; i++, now++) {
            in.demand = 40 + s; // Sample number s has demand 40 + s
            heatlog_add(&l, &in, now);
        }
    in.demand = 42;
    heatlog_add(&l, &in, now++); // Ends sample 1
    CHECK("read: two held", heatlog_begin(&l) == 0 && heatlog_end(&l) == 2);
    CHECK("read: from the start", heatlog_read(&l, 0, out, 5) == 2 && out[0].demand == 40 && out[1].demand == 41);
    CHECK("read: from a number", heatlog_read(&l, 1, out, 5) == 1 && out[0].demand == 41);
    CHECK("read: up to max", heatlog_read(&l, 0, out, 1) == 1 && out[0].demand == 40);
    CHECK("read: not yet kept", heatlog_read(&l, 2, out, 5) == 0);
    for (int s = 2; s < 5; s++)
        for (int i = 0; i < I; i++, now++) {
            in.demand = 40 + s;
            heatlog_add(&l, &in, now);
        }
    CHECK("read: ring wrapped", heatlog_begin(&l) == 2 && heatlog_end(&l) == 5);
    CHECK("read: overwritten", heatlog_read(&l, 1, out, 5) == 0);
    CHECK("read: across the wrap",
          heatlog_read(&l, 2, out, 5) == 3 && out[0].demand == 42 && out[1].demand == 43 && out[2].demand == 44);
    CHECK("read: the same samples as get", heatlog_get(&l, out, 5) == 3 && out[0].demand == 42);
}

static void test_heatout(void)
{
    // 518 m3/h warmed from 23.5 to 33.5 °C: 518 / 3600 m3/s x 1.2 kg/m3 x 1005 J/(kg K) x 10 K
    CHECK("heat output from air flow and rise", fabsf(heatlog_heatout(518, 33.5, 23.5) - 1735) < 2);
    CHECK("no air flow, no heat", heatlog_heatout(0, 33.5, 23.5) == 0);
    CHECK("a cold coil takes heat", heatlog_heatout(300, 10, 22) < 0);
    CHECK("unknown flow", isnan(heatlog_heatout(NAN, 33.5, 23.5)));
    CHECK("unknown out", isnan(heatlog_heatout(518, NAN, 23.5)));
    CHECK("unknown intake", isnan(heatlog_heatout(518, 33.5, NAN)));
}

int main(void)
{
    test_heatout();
    test_read();
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
