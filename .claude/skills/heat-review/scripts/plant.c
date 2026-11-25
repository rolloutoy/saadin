// Closed-loop plant for the heating controller: the firmware's own heatctl.c against the model of the house and the
// unit in graph-logs/model.json. Built and run by sim.py, which passes the model and the device's own settings (as
// pull.py read them from it, in the settings' units) in the environment; see
// references/model.md. One second per step, 72 h: a day to settle, then hours 24-48 are scored as "steady", and a
// change of heat need or of weather that starts at hour 48 and takes 2 h is scored over hours 48-60 as "change".
//
//   plant <outdoor> <gain W> [<outdoor2> <gain2 W>] [<trace from min> <trace to min>]
//
// If heatctl_params_t or heatctl_in_t changes in the firmware this file must follow: it is a test harness, so fix
// it here, never by bending the firmware. A new setting does not stop it building by itself (it would just be 0
// here), so the sizes are checked below; `make -C ESP test` builds this file to show it at the time of the change.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heatctl.h"

static float envf(const char *n, float d)
{
    const char *v = getenv(n);
    return v && *v ? atof(v) : d;
}

static const char *envs(const char *n, const char *d)
{
    const char *v = getenv(n);
    return v && *v ? v : d;
}

#define TABLE_DEFAULT "40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470"

// The controller's settings or inputs changed: give every new field its value in main() below (a setting's default
// is in settings.def, and sim.py's SETTING_ENV passes the device's own), then put the new sizes here
_Static_assert(sizeof(heatctl_params_t) == 428 && sizeof(heatctl_in_t) == 20,
               "heatctl_params_t or heatctl_in_t changed: plant.c must follow, see the comment above this line");

int main(int argc, char **argv)
{
    float out1 = argc > 1 ? atof(argv[1]) : -1, gain1 = argc > 2 ? atof(argv[2]) : 0;
    float out2 = argc > 3 ? atof(argv[3]) : out1, gain2 = argc > 4 ? atof(argv[4]) : gain1;
    int from = argc > 5 ? atoi(argv[5]) : 0, to = argc > 6 ? atoi(argv[6]) : 0;
    int changing = out2 != out1 || gain2 != gain1 || envf("PULSE_W", 0) != 0;
    // House
    float ca = envf("AIR_J_K", 4e6), kam = envf("AIR_MASS_W_K", 1000), cm = envf("MASS_J_K", 5e7),
          kmo = envf("MASS_OUT_W_K", 90);
    float senslag = envf("SENSLAG", 0), heatlag = envf("HEATLAG", 0); // Harsher plants: s of lag
    // Gain by time of day on top of the scenario's (W), the plant's clock starting at midnight
    float gday[3] = {envf("GAIN_06", 0), envf("GAIN_12", 0), envf("GAIN_18", 0)};
    // A burst of other heat (a sauna, an oven): PULSE_W for PULSE_MIN minutes from hour 48, scored as the change
    float pulse = envf("PULSE_W", 0), pulsemin = envf("PULSE_MIN", 120);
    // Defrost
    float frostmax = envf("FROST_WH", 3000), dsec = envf("DEFROST_S", 500), dq = envf("DEFROST_ROOM_W", -2500),
          dlead = envf("DEFROST_LEAD_S", 60), dpower = envf("DEFROST_POWER_W", 800);
    // Unit: what it really gives at each level, whatever the controller's table says
    float hpt = envf("HEAT_PER_TABLE", 3.5), ppt = envf("POWER_PER_TABLE", 1);
    float q100q = envf("Q100Q", 5000), p100q = envf("P100Q", 1500), q100 = envf("Q100", 7000),
          p100 = envf("P100", 2500);
    float ramplo = envf("RAMP_LOW", 1200) / 60, ramphi = envf("RAMP_HIGH", 350) / 60;
    heatctl_table_t unit;
    if (heatctl_parse_table(&unit, envs("UNIT_TABLE", TABLE_DEFAULT), TABLE_DEFAULT))
        return fprintf(stderr, "UNIT_TABLE does not parse\n"), 1;
    float q95 = 0, p95 = 0;
    for (int r = 0; r < unit.n; r++)
        if (unit.level[r].demand < 100 && unit.level[r].watts * hpt > q95)
            q95 = unit.level[r].watts * hpt, p95 = unit.level[r].watts * ppt;
    // Controller, as the device has it: what sim.py passes from device.json, else the default in settings.def.
    // Left out are settings that do nothing here: the setpoints sent to the unit, the fans, the defrost thresholds
    // on a liquid temperature the plant makes up, and target control's own setpoint steps
    float T = envf("TARGET", 21); // auto.t, before the curve
    heatctl_params_t p = {
        .target = T,
        .stale = 1800,
        .smooth = envf("SMOOTH", 0),
        .tmin = 16,
        .tmax = 32,
        .demand_set = 30,
        .demand_full = envf("FULL", 1),
        .demand_start = envf("START", 95), // Not the device's 60: only shortens the day the plant takes to settle
        .demand_rate = envf("RATE", 600),
        .demand_minrun = envf("MINRUN", 30) * 60,
        .cop = envf("COP", 3),
        .kp = envf("KP", 900),
        .ti = envf("TI", 90) * 60,
        .idle_temp = 16,
        .idle_over = envf("IDLE_OVER", 0.5),
        .idle_under = envf("IDLE_UNDER", 0),
        .idle_min = envf("IDLE_MIN", 30) * 60,
        .cold_enter = envf("COLD_ENTER", 2),
        .cold_exit = envf("COLD_EXIT", 5),
        .cold_hold = envf("COLD_HOLD", 60) * 60,
        .cold_set = 23,
        .cold_avg = 3 * 3600,
        .cold_band = 0.4,
        .cold_dwell = 3 * 3600,
        .defrost_enter = 5,
        .defrost_exit = 25,
        .defrost_max = 1800,
        .defrost_hold = envf("HOLD", 10) * 60,
        .defrost_holdmax = envf("HOLDMAX", 100),
        .defrost_watchdog = envf("WATCHDOG", 0),
        .defrost_loops = envf("LOOPS", 3),
        .defrost_window = envf("WINDOW", 90) * 60,
        .defrost_rest = envf("REST", 15) * 60,
        .idle_fan = HEATCTL_FAN_LEAVE,
        .demand_fan = HEATCTL_FAN_LEAVE,
        .cold_fan = HEATCTL_FAN_LEAVE,
    };
    const char *e = heatctl_parse_table(&p.table, envs("TABLE", TABLE_DEFAULT), TABLE_DEFAULT);
    if (e)
        return fprintf(stderr, "TABLE: %s\n", e), 1;
    e = heatctl_parse_curve(&p.curve, envs("CURVE", "")); // auto.curve: the target offset by outdoor temperature
    if (e)
        return fprintf(stderr, "CURVE: %s\n", e), 1;
    heatctl_t s = {0};
    T += heatctl_curve_offset(&p.curve, out1); // From here on the target in force at the start, for where to begin
    heatctl_in_t in = {.enabled = 1, .power = 1, .heat = 1, .room = T, .outdoor = out1, .liquid = 35, .heatout = 0};
    uint32_t now = 1000;
    heatctl_out_t o = heatctl_step(&s, &p, &in, now);
    float uatot = kam * kmo / (kam + kmo);
    float room = T, mass = T - uatot * (T - out1) / kam, q = 0, qa = 0, sens = T, frost = 0;
    int defrost = 0, level = -2, sub = -1, state = -1;
    // [0] steady, [1] change
    double se[2] = {0}, se2[2] = {0}, sq[2] = {0}, sp[2] = {0};
    long n[2] = {0}, chg[2] = {0}, big[2] = {0}, nd[2] = {0}, t100q[2] = {0}, t100[2] = {0}, tlow[2] = {0},
         tidle[2] = {0}, tcold[2] = {0};
    float lo[2] = {99, 99}, hi[2] = {-99, -99};
    double ws = 0, worst = 0, wsum = 0, ppsum = 0;
    long wn = 0, cycn = 0, ppn = 0;
    float clo = 99, chi = -99;
    for (uint32_t i = 0; i < 72 * 3600; i++) {
        float f = i < 48 * 3600 ? 0 : fminf((i - 48 * 3600) / 7200.0f, 1);
        float outdoor = out1 + (out2 - out1) * f, g = gain1 + (gain2 - gain1) * f;
        int hour = (i / 3600) % 24;
        g += hour >= 18 ? gday[2] : hour >= 12 ? gday[1] : hour >= 6 ? gday[0] : 0;
        if (pulse && i >= 48 * 3600 && i < 48 * 3600 + pulsemin * 60)
            g += pulse;
        float h = 0, pw = 0;
        if (o.state == HEATCTL_DEMAND || o.state == HEATCTL_COLD) {
            if (o.state == HEATCTL_COLD || (o.demand == 100 && !o.quiet))
                h = q100, pw = p100; // Target control's setpoint is not modelled: it gives all it has
            else if (o.demand == 100)
                h = q100q, pw = p100q;
            else
                for (int r = 0; r < unit.n; r++)
                    if (unit.level[r].demand == o.demand)
                        h = unit.level[r].watts * hpt, pw = unit.level[r].watts * ppt;
        }
        // Defrost once enough heat has been given: the output falls first, then the liquid, then it is over
        if (!defrost && frost >= frostmax && h > 0)
            defrost = 1;
        if (defrost) {
            defrost++;
            h = 0;
        }
        in.liquid = defrost > dlead ? -20 : 35;
        if (defrost > dlead + dsec) {
            defrost = 0;
            frost = 0;
            in.liquid = 30;
        }
        if (h < q)
            q = h;
        else
            q += fminf(h - q, q < q95 ? ramplo : ramphi);
        // Power follows the heat being given: as up to 95% while under that level's heat, then on a line to the top
        float pel = defrost    ? dpower
                    : q <= q95 ? fmaxf(q * p95 / q95, 100)
                               : p95 + (pw - p95) * (q - q95) / fmaxf(h - q95, 1);
        if (!defrost && pel > pw)
            pel = pw;
        frost += q / 3600;
        in.heatout = q;
        float qroom = defrost > dlead ? dq : q;
        qa = heatlag > 0 ? qa + (qroom - qa) / heatlag : qroom;
        float droom = (kam * (mass - room) + qa + g) / ca;
        mass += (kam * (room - mass) + kmo * (outdoor - mass)) / cm;
        room += droom;
        sens = senslag > 0 ? sens + (room - sens) / senslag : room;
        in.room = roundf(sens * 100) / 100;
        in.outdoor = outdoor;
        o = heatctl_step(&s, &p, &in, ++now);
        int k = i >= 60 * 3600 ? -1 : i >= 48 * 3600 ? 1 : i >= 24 * 3600 ? 0 : -1;
        int top = o.state == HEATCTL_DEMAND && o.demand == 100;
        if (k >= 0) {
            float er = room - o.target;
            se[k] += er, se2[k] += er * er, sq[k] += q, sp[k] += pel, n[k]++;
            lo[k] = fminf(lo[k], room), hi[k] = fmaxf(hi[k], room);
            if (s.level != level && level >= 0 && state == HEATCTL_DEMAND && o.state == HEATCTL_DEMAND) {
                chg[k]++;
                if (top || p.table.level[level].demand == 100)
                    big[k]++;
            }
            if (o.sub == HEATCTL_SUB_DEFROST && sub != HEATCTL_SUB_DEFROST) {
                nd[k]++;
                if (k == 0 && cycn) // Swing of the room within each defrost cycle
                    ppsum += chi - clo, ppn++;
                clo = 99, chi = -99, cycn = 1;
            }
            if (k == 0)
                clo = fminf(clo, room), chi = fmaxf(chi, room);
            t100q[k] += top && o.quiet && !defrost, t100[k] += top && !o.quiet && !defrost;
            tlow[k] += room < o.target - 0.3f, tidle[k] += o.state == HEATCTL_IDLE, tcold[k] += o.state == HEATCTL_COLD;
        }
        if (i >= 48 * 3600) { // 3 h means after the change
            ws += room - o.target;
            if (++wn == 3 * 3600) {
                double m = ws / wn;
                if (fabs(m) > fabs(worst))
                    worst = m;
                wsum += fabs(m), ws = 0, wn = 0;
            }
        }
        if (to && (int)(i / 60) >= from && (int)(i / 60) < to &&
            (s.level != level || (int)o.sub != sub || i % 300 == 0))
            printf(
                "t %6.1f %s %-7s dem %3d%s room %.2f P %+5.0f I %5.0f req %5.0f tgt %5.0f heat %5.0f deficit %5.0f\n",
                i / 60.0 - from, heatctl_state_name(o.state), o.sub ? heatctl_sub_name(o.sub) : "-", o.demand,
                o.demand == 100 && o.quiet ? "q" : " ", room, p.kp * (o.target - in.room), o.integral, o.heatw,
                o.heattarget, q, o.deficit);
        level = s.level, sub = o.sub, state = o.state;
    }
    for (int k = 0; k < 2; k++) {
        if (k && !changing)
            break;
        double hrs = n[k] / 3600.0;
        printf("%s rms=%.3f mean=%.3f lo=%.2f hi=%.2f low_min_h=%.1f heat=%.0f power=%.0f cop=%.2f defrost_min=%.0f "
               "chg_h=%.2f big_h=%.2f at100q=%.3f at100=%.3f idle=%.3f cold=%.3f",
               k ? "change" : "steady", sqrt(se2[k] / n[k]), se[k] / n[k], lo[k], hi[k], 60.0 * tlow[k] / n[k],
               sq[k] / n[k], sp[k] / n[k], sq[k] / sp[k], nd[k] ? 60 * hrs / nd[k] : 0, chg[k] / hrs, big[k] / hrs,
               (double)t100q[k] / n[k], (double)t100[k] / n[k], (double)tidle[k] / n[k], (double)tcold[k] / n[k]);
        if (k)
            printf(" worst3h=%.3f sum3h=%.3f", worst, wsum);
        else
            printf(" swing=%.2f", ppn ? ppsum / ppn : 0);
        printf("\n");
    }
    return 0;
}
