#!/usr/bin/env python3
"""Run the firmware's heating controller against the model, to check the model and to try changes before the device.

Usage: python3 -I sim.py compare [log]
       python3 -I sim.py sweep [--scen S]... [--var "label:KEY=V ..."]... [--grid KEY=a,b,c]... [--robust]
       python3 -I sim.py trace --scen S [--var ...] [--from MIN] [--to MIN]

  compare   closed loop against a log: for each stretch of the log, run the plant in the same weather with the
            device's settings and set the two side by side. This is the test of whether the model may be trusted
            for a sweep.
  sweep     scenarios x variants. A variant sets controller settings in the settings' own units (TARGET CURVE KP TI
            RATE HOLD HOLDMAX PUSH PUSHMAX PUSHALT SMOOTH TABLE COP FULL MINRUN IDLE_OVER IDLE_UNDER IDLE_MIN COLD_ENTER COLD_EXIT
            COLD_HOLD WATCHDOG LOOPS WINDOW REST) or plant figures; SRC=<dir with heatctl.c> runs another source
            tree (a worktree with a code change), DEFS="-DX" passes compiler flags; PULSE_W=900 PULSE_MIN=120 adds a
            burst of other heat (a sauna) after two settled days and scores the 12 h from it. The first row is
            always the device as it is: the settings pull.py read from it, kept in device.json.
  trace     what the plant does minute by minute.

A scenario is OUTDOOR[,GAIN][>OUTDOOR2[,GAIN2]]: outdoor degC and extra heat in the house in W (negative for a
loss); with ">" the weather or the gain changes over two hours after a settled start. OUTDOOR,GAIN,day adds the
model's fitted gain by time of day, which is the disturbance the house really has. Defaults cover -11 to +4 degC,
a day at -1, and three changes; add ones near the current weather.
"""
import argparse
import hashlib
import itertools
import os
import shlex
import statistics as st
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402
import model as modelmod  # noqa: E402
from heatlog import stamp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
# The settings the plant's controller takes from device.json, in the settings' own units (plant.c converts)
SETTING_ENV = {"auto.t": "TARGET", "auto.curve": "CURVE", "auto.smooth": "SMOOTH",
               "pid.kp": "KP", "pid.ti": "TI", "demand.rate": "RATE", "demand.table": "TABLE", "demand.cop": "COP",
               "demand.full": "FULL", "demand.minrun": "MINRUN",
               "idle.over": "IDLE_OVER", "idle.under": "IDLE_UNDER", "idle.min": "IDLE_MIN",
               "cold.enter": "COLD_ENTER", "cold.exit": "COLD_EXIT", "cold.hold": "COLD_HOLD",
               "defrost.hold": "HOLD", "defrost.holdmax": "HOLDMAX", "defrost.push": "PUSH",
               "defrost.pushmax": "PUSHMAX", "defrost.pushalt": "PUSHALT", "defrost.watchdog": "WATCHDOG",
               "defrost.loops": "LOOPS", "defrost.window": "WINDOW", "defrost.rest": "REST"}
DEFAULT_SCEN = ["-11", "-6", "-1", "4", "-1,0,day", "-1,0>-1,500", "4,0>4,-500", "-6,0>-6,-700"]
ROBUST = [("sensor lag 5 min", {"SENSLAG": 300}), ("sensor lag 10 min", {"SENSLAG": 600}),
          ("heat reaches the room over 10 min", {"HEATLAG": 600}),
          ("half the air mass, sensor lag 5 min", {"AIR_SCALE": 0.5, "SENSLAG": 300})]


def build(src=None, defs=""):
    src = os.path.abspath(src or os.path.join(heatlog.repo_root(), "ESP", "main"))
    files = [os.path.join(HERE, "plant.c"), os.path.join(src, "heatctl.c"), os.path.join(src, "heatctl.h")]
    for f in files:
        if not os.path.exists(f):
            raise SystemExit("cannot build the plant: %s is missing" % f)
    key = hashlib.sha1(("|".join("%s:%s" % (f, os.path.getmtime(f)) for f in files) + defs).encode()).hexdigest()[:12]
    out = os.path.join(heatlog.logdir(), ".build")
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "plant-" + key)
    if not os.path.exists(exe):
        cmd = ["gcc", "-O2", "-std=gnu99", "-I", src, "-o", exe, files[0], files[1], "-lm"] + defs.split()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("the plant does not build against %s:\n%s\nIf the firmware's controller interface changed,"
                             " fix plant.c to follow it." % (src, r.stderr[-3000:]))
    return exe


def plant_env(m, outdoor):
    h, d, u = m["house"], m["defrost"], m["unit"]
    at = modelmod.at
    return {"AIR_J_K": h["air_J_K"], "AIR_MASS_W_K": h["air_mass_W_K"], "MASS_J_K": h["mass_J_K"],
            "MASS_OUT_W_K": h["mass_out_W_K"], "FROST_WH": d["frost_Wh"], "DEFROST_S": d["seconds"],
            "DEFROST_ROOM_W": d["room_W"], "DEFROST_LEAD_S": d["lead_s"], "DEFROST_POWER_W": d.get("power_W", 800),
            "HEAT_PER_TABLE": at(u["heat_per_table"], outdoor), "POWER_PER_TABLE": u["power_per_table"],
            "Q100Q": at(u["q100q"], outdoor), "P100Q": at(u["p100q"], outdoor), "Q100": at(u["q100"], outdoor),
            "P100": at(u["p100"], outdoor), "RAMP_LOW": u["ramp_low_W_min"], "RAMP_HIGH": u["ramp_high_W_min"],
            "UNIT_TABLE": u.get("table") or heatlog.DEFAULT_TABLE}


def device_env():
    device = heatlog.read_json(os.path.join(heatlog.logdir(), "device.json"), {}) or {}
    env = {SETTING_ENV[k]: v for k, v in (device.get("settings") or {}).items() if k in SETTING_ENV}
    if not env.get("TARGET") and device.get("target"):
        # No auto.t on record, only the effective target at the last pull: the curve's offset is in that already
        env["TARGET"] = device["target"]
        env.pop("CURVE", None)
    return env


def parse_scen(text):
    a, _, b = text.partition(">")

    day = a.endswith(",day")
    if day:
        a = a[:-4]

    def one(x):
        p = x.split(",")
        return float(p[0]), float(p[1]) if len(p) > 1 else 0.0
    o1, g1 = one(a)
    o2, g2 = one(b) if b else (o1, g1)
    return o1, g1, o2, g2, day


def run(m, scen, env, src=None, defs="", trace=None):
    o1, g1, o2, g2 = scen[:4]
    full = plant_env(m, (o1 + o2) / 2)
    if len(scen) > 4 and scen[4]:  # The fitted gain by time of day, as the disturbance
        daily = (m.get("gains") or {}).get("daily_W") or {}
        full.update({"GAIN_06": daily.get("06-12", 0), "GAIN_12": daily.get("12-18", 0), "GAIN_18": daily.get("18-24", 0)})
    full.update(device_env())
    full.update(env)
    if "AIR_SCALE" in full:
        full["AIR_J_K"] = float(full["AIR_J_K"]) * float(full.pop("AIR_SCALE"))
    src = full.pop("SRC", src)
    defs = full.pop("DEFS", defs)
    # The plant gets these and nothing else: its names are plain ones (TARGET, COP) that a shell may well have set
    e = {k: str(v) for k, v in full.items()}
    args = [build(src, defs), str(o1), str(g1), str(o2), str(g2)] + ([str(trace[0]), str(trace[1])] if trace else [])
    r = subprocess.run(args, capture_output=True, text=True, env=e)
    if r.returncode:
        raise SystemExit("plant failed: %s" % r.stderr)
    out = {}
    for line in r.stdout.splitlines():
        w = line.split()
        if w and w[0] in ("steady", "change"):
            out[w[0]] = {k: float(v) for k, v in (x.split("=") for x in w[1:])}
        elif trace:
            print(line)
    return out


def parse_var(text):
    label, _, rest = text.partition(":")
    env = {}
    for part in shlex.split(rest):
        k, _, v = part.partition("=")
        env[k] = v
    return label, env


def fmt_steady(r):
    return ("rms %.2f  %4.1f min/h low  room %.2f..%.2f  swing %.2f | COP %.2f  defrost /%3.0f min | changes %.1f/h, "
            "big %.1f/h | 100q %2.0f%%  100 %2.0f%%%s" % (
                r["rms"], r["low_min_h"], r["lo"], r["hi"], r["swing"], r["cop"], r["defrost_min"], r["chg_h"],
                r["big_h"], 100 * r["at100q"], 100 * r["at100"],
                "  idle %2.0f%%" % (100 * r["idle"]) if r["idle"] > 0.005 else ""))


def fmt_change(r):
    return ("rms %.2f  room %.2f..%.2f  %4.1f min/h low  worst 3 h mean %+.2f  sum |3 h means| %.2f | COP %.2f | "
            "changes %.1f/h, big %.1f/h | 100q %2.0f%%  100 %2.0f%%%s" % (
                r["rms"], r["lo"], r["hi"], r["low_min_h"], r["worst3h"], r["sum3h"], r["cop"], r["chg_h"], r["big_h"],
                100 * r["at100q"], 100 * r["at100"], "  idle %2.0f%%" % (100 * r["idle"]) if r["idle"] > 0.005 else ""))


def cmd_sweep(args):
    m = modelmod.load_model()
    variants = [("device as it is", {})] + [parse_var(v) for v in args.var or []]
    if args.grid:
        keys = [g.split("=")[0] for g in args.grid]
        vals = [g.split("=")[1].split(",") for g in args.grid]
        for combo in itertools.product(*vals):
            variants.append((" ".join("%s=%s" % kv for kv in zip(keys, combo)), dict(zip(keys, combo))))
    scens = args.scen or DEFAULT_SCEN
    width = max(len(v[0]) for v in variants) + 1
    print("Model: %s. Device settings: %s" % (
        modelmod.fit_label(m),
        " ".join("%s=%s" % kv for kv in sorted(device_env().items()) if kv[0] != "TABLE") or "defaults"))
    print("big = steps to or from 100q/100. low = more than 0.3 degC under target. Differences under about 0.04 degC "
          "rms are within what the phase of the defrost cycle alone moves.\n")
    for text in scens:
        scen = parse_scen(text)
        changing = scen[0] != scen[2] or scen[1] != scen[3]
        print("== %s" % (("outdoor %+g degC, gain %+g W, then to %+g degC, %+g W over 2 h" % scen[:4]) if changing else
                         ("%s, outdoor %+g degC, gain %+g W" % ("a day with the fitted gain by time of day"
                                                                 if scen[4] else "steady", scen[0], scen[1]))))
        for label, env in variants:
            r = run(m, scen, env, args.src, args.defs or "")
            pulsed = "change" in r and not changing  # A variant with PULSE_W: the 12 h from the burst
            print("%-*s %s" % (width, label, fmt_change(r["change"]) if changing or pulsed else fmt_steady(r["steady"])))
            if args.robust:
                key = "change" if changing or pulsed else "steady"
                rows = [(name, run(m, scen, dict(env, **extra), args.src, args.defs or "")[key]) for name, extra in ROBUST]
                worst = max(rows, key=lambda x: x[1]["rms"])
                print("%-*s   harsher plants: rms %s; worst is \"%s\": room %.2f..%.2f, big %.1f/h" % (
                    width, "", " ".join("%.2f" % x[1]["rms"] for x in rows), worst[0], worst[1]["lo"], worst[1]["hi"],
                    worst[1]["big_h"]))
        print()


def blocks(s):
    """Stretches of whole defrost cycles in about the same weather, about six hours each"""
    out, cur = [], []
    for c in heatlog.cycle_stats(s):
        if c["demand_time"] < 0.9 or c["heat"] is None:
            if cur:
                out.append(cur)
            cur = []
            continue
        if cur and (abs(c["outdoor"] - cur[0]["outdoor"]) > 2.5 or sum(x["minutes"] for x in cur) >= 330 or
                    c["t"] - (cur[-1]["t"] + cur[-1]["minutes"] * 60) > 120):
            out.append(cur)
            cur = []
        cur.append(c)
    if cur:
        out.append(cur)
    return [b for b in out if len(b) >= 2]


def cmd_compare(args):
    m = modelmod.load_model()
    s = heatlog.find_log(args.log)
    print("Closed loop, model against %s (model %s)" % (s.name, modelmod.fit_label(m)))
    print("For each stretch the plant runs steady in the same weather, with extra heat in the house (\"gain\") set "
          "so that it needs the heat the log shows.\n")
    eff = s.col("effective")
    bad = total = 0
    for b in blocks(s):
        W = sum(c["minutes"] for c in b)

        def wmean(k):
            return sum(c[k] * c["minutes"] for c in b) / W
        # The target as set in the stretch, for the plant to add the curve to; a log without it gives only the
        # effective target, with the curve's offset in it
        base = [x for x in s.col("target")[b[0]["a"]:b[-1]["b"]] if x is not None]
        target = {"TARGET": st.mean(base)} if base else {
            "TARGET": st.mean([x for x in eff[b[0]["a"]:b[-1]["b"]] if x is not None]), "CURVE": ""}
        log = {"outdoor": wmean("outdoor"), "heat": wmean("heat"), "defrost_min": st.median([c["minutes"] for c in b]),
               "at100q": wmean("at100q"), "at100": wmean("at100"), "swing": st.median([c["hi"] - c["lo"] for c in b]),
               "rms": (sum(c["rms"] ** 2 * c["minutes"] for c in b) / W) ** 0.5, "cop": wmean("heat") / wmean("power"),
               "chg_h": sum(c["changes"] for c in b) / (W / 60.0), "err": wmean("err")}
        gain, r = 0.0, None
        for _ in range(4):  # The plant's heat falls one for one with the gain, near enough
            r = run(m, (log["outdoor"], gain, log["outdoor"], gain), target)["steady"]
            if abs(r["heat"] - log["heat"]) < 0.01 * log["heat"]:
                break
            gain += r["heat"] - log["heat"]
        transient = abs(log["err"]) > 0.25
        short = sum(1 for c in b if c["minutes"] < heatlog.SHORT_CYCLE)
        loopy = short >= 0.25 * len(b)
        print("%s to %s, outdoor %+.1f degC, %d cycles%s" % (
            stamp(b[0]["t"]), stamp(b[-1]["t"] + b[-1]["minutes"] * 60), log["outdoor"], len(b),
            "  [room %+.2f off target on average: not settled, not counted]" % log["err"] if transient else
            "  [%d short cycles: the plant has no defrost loops, not counted]" % short if loopy else ""))
        print("  gain needed %+.0f W for the model to need the log's %.0f W%s" % (
            gain, log["heat"], "  <-- the house model is off here, or the house had this much other heat or loss"
            if abs(gain) > 300 else ""))
        for label, key, tol, scale, unit in (("defrost every", "defrost_min", 0.2, 1, "min"),
                                             ("time at 100q", "at100q", 8, 100, "%"),
                                             ("time at plain 100", "at100", 5, 100, "%"),
                                             ("room swing per cycle", "swing", 0.2, 1, "degC"),
                                             ("rms room error", "rms", 0.08, 1, "degC"), ("COP", "cop", 0.25, 1, ""),
                                             ("demand changes per hour", "chg_h", 1.0, 1, "")):
            a, c = log[key] * scale, r[key] * scale
            ok = abs(c - a) <= (tol * a if key == "defrost_min" else tol)
            if not transient and not loopy:
                total += 1
                bad += not ok
            print("    %-24s log %6.2f  model %6.2f %-4s [%s]" % (label, a, c, unit, "ok" if ok else "OFF"))
        print()
    if not total:
        print("no stretch of two or more whole defrost cycles in demand control: nothing to compare")
    else:
        print("%d of %d figures off in the stretches that count (settled, no defrost loops). With more than about one "
              "in five off, do not trust a sweep until the model is refitted or the cause is understood (see "
              "references/model.md)." % (bad, total))


def cmd_trace(args):
    m = modelmod.load_model()
    for text in args.scen or ["-1"]:
        for label, env in [("device as it is", {})] + [parse_var(v) for v in args.var or []]:
            print("== %s, %s" % (text, label))
            r = run(m, parse_scen(text), env, args.src, args.defs or "", trace=(args.frm, args.to))
            print("   " + fmt_steady(r["steady"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["compare", "sweep", "trace"])
    ap.add_argument("log", nargs="?")
    ap.add_argument("--scen", action="append")
    ap.add_argument("--var", action="append")
    ap.add_argument("--grid", action="append")
    ap.add_argument("--src", help="directory with heatctl.c and heatctl.h (default ESP/main of this checkout)")
    ap.add_argument("--defs", help="extra compiler flags, e.g. \"-DTRY_X\"")
    ap.add_argument("--robust", action="store_true", help="sweep: also run each row on four harsher plants")
    ap.add_argument("--from", dest="frm", type=int, default=1800, help="trace: first minute (default 1800)")
    ap.add_argument("--to", type=int, default=1950)
    args = ap.parse_args()
    {"compare": cmd_compare, "sweep": cmd_sweep, "trace": cmd_trace}[args.command](args)


if __name__ == "__main__":
    main()
