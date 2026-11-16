#!/usr/bin/env python3
"""What a heat log shows, as text tables.

Usage: python3 -I report.py <command> [log] [options]     (log: a file in graph-logs/, default the newest)

  check     data sanity, the controller's own bookkeeping, and consistency with the settings on record
  cycles    one row per defrost cycle: room error, heat, COP, time at 100%
  levels    settled power and heat at each demand level, over ALL saved logs, against demand.table
  steps     every demand step: the deficit, the rule's threshold, the room, and predicted against actual heat
  defrost   the unit and the room through the minutes after a defrost, by the level held, and frost per cycle,
            with the humidity and wind of each cycle where weather.py has them on record
  trace     samples between two times:  trace [log] --from "01-13 07:18" --to "01-13 09:00" [--every 8]
"""
import argparse
import datetime
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402
import weather  # noqa: E402
from heatlog import DEFROST, HOLD, stamp  # noqa: E402

OK, WARN, FAIL = "ok  ", "WARN", "FAIL"


def debt(s, i):
    """Defrost heat still to be made up (Wh): the heat target is the request plus this over two hours"""
    return 2.0 * (s.col("heattarget")[i] - s.col("heatw")[i])


def books(s):
    """Per cycle: request, target, output (Wh), and how far deficit + debt is from request - output"""
    H = s.interval / 3600.0
    req, tgt, out, defi = s.col("heatw"), s.col("heattarget"), s.col("heatout"), s.col("deficit")
    rows = []
    state = s.col("state")
    for a, b in s.cycles():
        # Whole cycles in demand control only: the deficit starts again at zero whenever demand control is entered
        if any(x != heatlog.DEMAND for x in state[a:b + 1]):
            continue
        if any(x is None for x in req[a:b + 1] + tgt[a:b + 1] + out[a:b + 1] + defi[a:b + 1]):
            continue
        R, T, O = sum(req[a:b]) * H, sum(tgt[a:b]) * H, sum(out[a:b]) * H
        dd, db = defi[b] - defi[a], debt(s, b) - debt(s, a)
        ends = (s.table[0][0], s.table[-1][0])  # The firmware drops what the lowest or top level cannot make up
        rows.append({"t": s.t[a], "minutes": (b - a) * s.interval / 60.0, "request": R, "target": T, "output": O,
                     "ddeficit": dd, "ddebt": db, "off": (dd + db) - (R - O), "lo": min(defi[a:b]),
                     "hi": max(defi[a:b]), "at_end": any(s.level(i) in ends for i in range(a, b))})
    return rows


def demand_rate():
    """demand.rate as on record in device.json (s), the firmware's default when it is not"""
    device = heatlog.read_json(os.path.join(heatlog.logdir(), "device.json"), {}) or {}
    try:
        return float((device.get("settings") or {}).get("demand.rate") or 600)
    except ValueError:
        return 600.0


def step_rows(s):
    """Every demand step outside a defrost, with the rule's own numbers rebuilt from the log"""
    if not s.has("deficit"):
        return []
    rate = demand_rate()
    tw = s.table_watts()
    names = [r[0] for r in s.table]
    out, rows = s.col("heatout"), []
    k8, k2 = int(480 / s.interval), int(120 / s.interval)
    starts = [a for a, _ in s.defrosts()]
    for i in range(1, s.n):
        lv, nx = s.level(i - 1), s.level(i)
        if lv is None or nx is None or lv == nx or s.col("sub")[i] == DEFROST or lv not in tw or nx not in tw:
            continue
        scale, tgt = s.col("scale")[i - 1], s.col("heattarget")[i - 1]
        if scale is None or tgt is None:
            continue
        up = tw[nx] > tw[lv]
        j = names.index(lv) + (1 if up else -1)
        adj = names[j] if 0 <= j < len(names) else nx
        # Half of what the next level would over-give (or under-give) in demand.rate, as the firmware has it
        th = (max(tw[adj] * scale - tgt, 0) if up else -max(tgt - tw[adj] * scale, 0)) * rate / 7200.0
        act = [x for x in out[i + k2:i + k8] if x is not None]
        nxt = min([a for a in starts if a >= i] or [s.n])
        rows.append({"i": i, "t": s.t[i], "from": lv, "to": nx, "up": up, "deficit": s.col("deficit")[i - 1],
                     "threshold": th, "target": tgt, "pred": tw[nx] * scale,
                     "actual": sum(act) / len(act) if len(act) > k2 else None,
                     "roomerr": (s.col("room")[i - 1] or 0) - (s.col("effective")[i - 1] or 0),
                     "to_defrost": (nxt - i) * s.interval / 60.0 if nxt < s.n else None, "rate": rate})
    return rows


def inferred(s):
    """Settings the log itself shows: the hold after a defrost and the shortest stay at a level"""
    sub, out = s.col("sub"), {}
    holds, a = [], None
    for i in range(s.n):
        if sub[i] == HOLD and a is None:
            a = i
        elif sub[i] != HOLD and a is not None:
            if sub[i] == 0:
                holds.append((i - a) * s.interval / 60.0)
            a = None
    if holds:
        out["defrost.hold"] = st.median(holds)
    stays, last = [], None
    for i in range(1, s.n):
        if s.level(i) != s.level(i - 1) and s.level(i) and s.level(i - 1):
            if last is not None:
                stays.append((i - last) * s.interval)
            last = i
        elif s.level(i) is None:
            last = None
    if len(stays) >= 3:
        out["demand.rate"] = min(stays)
    for col, name in (("kp", "pid.kp"), ("ti", "pid.ti")):
        v = [x for x in s.col(col) if x is not None]
        if v:
            out[name] = v[-1]
    return out


def cmd_check(s, args):
    res = []

    def add(status, text):
        res.append((status, text))

    raw = getattr(s, "raw", {})
    # --- the data itself
    gaps = [(s.t[i - 1], s.t[i] - s.t[i - 1]) for i in range(1, s.n) if s.t[i] - s.t[i - 1] > 2.5 * s.interval]
    add(OK if s.n > 240 else WARN, "%d samples every %d s, %s to %s (%.1f h)" % (
        s.n, s.interval, stamp(s.t[0]), stamp(s.t[-1]), (s.t[-1] - s.t[0]) / 3600.0))
    if any(s.t[i] <= s.t[i - 1] for i in range(1, s.n)):
        add(FAIL, "time does not run forward at every sample")
    add(WARN if gaps else OK, "%d gaps in the samples%s" % (len(gaps), "".join(
        "; %s for %.0f min" % (stamp(t), g / 60) for t, g in gaps[:4])))
    for key, lo, hi in (("room", 5, 35), ("outdoor", -40, 40), ("power", 0, 4500), ("liquid", -45, 75),
                        ("comp", 0, 140), ("heatout", -1500, 9000)):
        v = s.col(key)
        missing = sum(1 for x in v if x is None)
        bad = sum(1 for x in v if x is not None and not lo <= x <= hi)
        if key in ("room", "outdoor", "power") or s.has(key):
            st_ = FAIL if bad else WARN if missing > 0.02 * s.n else OK
            add(st_, "%s: %d missing, %d outside %s..%s" % (key, missing, bad, lo, hi))
    room = s.col("room")
    flat, run, at = 0, 0, None
    for i in range(1, s.n):
        run = run + 1 if room[i] is not None and room[i] == room[i - 1] else 0
        if run > flat:
            flat, at = run, i
    add(WARN if flat * s.interval > 2700 else OK, "room reading unchanged for at most %.0f min%s" % (
        flat * s.interval / 60.0, " (until %s): a sensor that stopped reporting?" % stamp(s.t[at])
        if flat * s.interval > 2700 else ""))
    share = {}
    for i in range(s.n):
        name = s.states[s.col("state")[i]] if s.col("state")[i] is not None else "?"
        share[name] = share.get(name, 0) + 1
    add(WARN if share.get("nosensor") else OK, "time by state: " + ", ".join(
        "%s %.0f%%" % (k, 100.0 * v / s.n) for k, v in sorted(share.items(), key=lambda x: -x[1])))

    # --- settings: what the log shows against what is on record
    device = heatlog.read_json(os.path.join(heatlog.logdir(), "device.json"), {}) or {}
    known = device.get("settings", {})
    for name, value in sorted(inferred(s).items()):
        rec = known.get(name)
        if rec is None:
            add(WARN, "%s is %.4g by the log, nothing on record in device.json" % (name, value))
        elif abs(float(rec) - value) > 0.1 * max(abs(value), 1):
            add(WARN, "%s is %.4g by the log, device.json says %s: the setting was changed during or after this "
                "log (device.json is what the device runs on now)" % (name, value, rec))
        else:
            add(OK, "%s is %.4g by the log, as on record" % (name, value))
    for col, label in (("kp", "pid.kp"), ("ti", "pid.ti"), ("target", "target")):
        v = s.col(col)
        ch = [(s.t[i], v[i - 1], v[i]) for i in range(1, s.n) if v[i] is not None and v[i - 1] is not None and
              v[i] != v[i - 1]]
        if ch:
            add(WARN if col != "target" else OK, "%s changed in the log: %s" % (label, "; ".join(
                "%s %.4g -> %.4g" % (stamp(t), a, b) for t, a, b in ch[:6])))
    if raw.get("table") and known.get("demand.table") and raw["table"] != known["demand.table"]:
        add(WARN, "demand.table in the log differs from device.json")

    # --- the controller's bookkeeping
    if s.has("deficit"):
        rows = books(s)
        inside = [r for r in rows if not r["at_end"]]
        if inside:
            worst = max(abs(r["off"]) for r in inside)
            add(OK if worst <= 15 else FAIL, "heat deficit books: deficit + defrost debt follows request - output to "
                "within %.0f Wh per cycle over %d cycles (%d more touched the lowest or top level, where the firmware "
                "drops what it cannot make up)" % (worst, len(inside), len(rows) - len(inside)))
        if rows:
            settled = rows[3:] if len(rows) > 5 else rows  # The make-up store fills over the first cycles
            R, O = sum(r["request"] for r in settled), sum(r["output"] for r in settled)
            off = [100 * (r["output"] - r["request"]) / r["request"] for r in settled]
            add(OK if abs(O - R) < 0.03 * R else WARN, "delivery: output is %+.1f%% of the request over %d cycles "
                "(single cycles %+.1f%% to %+.1f%%)" % (100 * (O - R) / R, len(settled), min(off), max(off)))
            big = [r for r in settled if abs(r["output"] - r["request"]) > 0.06 * r["request"]]
            for r in big[:5]:
                add(WARN, "  cycle from %s delivered %+.1f%% against the request" % (
                    stamp(r["t"]), 100 * (r["output"] - r["request"]) / r["request"]))
            lo, hi = min(r["lo"] for r in rows), max(r["hi"] for r in rows)
            add(OK if hi - lo < 900 else WARN, "heat deficit stays within %.0f..%.0f Wh" % (lo, hi))
        steps = step_rows(s)
        if steps:
            def med(sel):
                v = [100 * (r["actual"] / r["pred"] - 1) for r in steps if sel(r) and r["actual"] and r["pred"]]
                return (st.median(v), len(v)) if v else (None, 0)
            for label, sel in (("up to 100q", lambda r: r["to"] == "100q" and r["up"]),
                               ("down from 100q", lambda r: r["from"] == "100q" and not r["up"]),
                               ("below 100q", lambda r: "100" not in r["from"] + r["to"])):
                m, n = med(sel)
                if n:
                    add(OK if abs(m) < 12 else WARN, "heat predicted for the next level, steps %s: actual is %+.0f%% "
                        "of the prediction (median of %d)" % (label, m, n))
            against = [r for r in steps if (r["up"] and r["roomerr"] > 0.05) or (not r["up"] and r["roomerr"] < -0.05)]
            add(OK if len(against) <= 0.1 * len(steps) else WARN, "%d of %d steps went against the room (up while over "
                "target, or down while under)%s" % (len(against), len(steps), "".join(
                    "; %s %s>%s at %+.2f" % (stamp(r["t"]), r["from"], r["to"], r["roomerr"]) for r in against[:6])))
            late = [r for r in steps if r["up"] and r["to_defrost"] is not None and r["to_defrost"] < 4]
            add(OK if not late else WARN, "%d steps up within 4 min before a defrost (the unit winding down reads as "
                "a shortfall)%s" % (len(late), "".join("; %s" % stamp(r["t"]) for r in late[:6])))

    # --- demand table against what the unit drew (this log only; `levels` has all logs)
    tw = s.table_watts()
    for g in sorted(heatlog.level_stats([s]), key=lambda g: (tw.get(g["level"], 0), g["band"])):
        if g["minutes"] < 30 or g["level"] not in tw:
            continue
        off = 100 * (g["power"] / tw[g["level"]] - 1)
        add(OK if abs(off) <= 10 else WARN, "level %s (fan %s, outdoor %+.0f): %.0f W settled against %.0f W in "
            "demand.table (%+.0f%%), %.0f min" % (g["level"], g["fan"], g["outdoor"], g["power"], tw[g["level"]], off,
                                                  g["minutes"]))
        if g["cop"] is not None and not 1.5 <= g["cop"] <= 6:
            add(WARN, "  heat estimate at level %s gives COP %.2f: outside 1.5..6" % (g["level"], g["cop"]))

    # --- defrosts
    d = s.defrosts()
    if d:
        dur = [(b - a) * s.interval / 60.0 for a, b in d if b < s.n]
        iv = [(s.t[b[0]] - s.t[a[0]]) / 60.0 for a, b in zip(d, d[1:])]
        add(OK if not dur or max(dur) <= 15 else WARN, "%d defrosts: %.1f min each (longest %.1f)%s" % (
            len(d), st.median(dur) if dur else 0, max(dur) if dur else 0,
            ", %.0f min apart (%.0f..%.0f)" % (st.median(iv), min(iv), max(iv)) if iv else ""))
        loops = [s.t[d[i][0]] for i in range(len(d) - 3) if s.t[d[i + 3][0]] - s.t[d[i][0]] < 90 * 60]
        add(OK if not loops else WARN, "defrost loops (4 starts within 90 min): %d%s" % (
            len(loops), "".join("; from %s" % stamp(t) for t in loops[:4])))

    # --- control
    cs = heatlog.cycle_stats(s)
    if cs:
        off = [c for c in cs if abs(c["err"]) > 0.2 and c["demand_time"] > 0.9]
        runs_, run = 0, 0
        for c in cs:
            run = run + 1 if abs(c["err"]) > 0.2 else 0
            runs_ = max(runs_, run)
        add(OK if runs_ < 2 else WARN, "room: mean error per defrost cycle %+.2f..%+.2f degC; %d of %d cycles over 0.2 "
            "off, at most %d in a row" % (min(c["err"] for c in cs), max(c["err"] for c in cs), len(off), len(cs), runs_))
    for status, text in res:
        print(status, text)
    n = sum(1 for x in res if x[0] != OK)
    print("\n%d checks, %d need a look" % (len(res), n))


def cmd_cycles(s, args):
    cs = heatlog.cycle_stats(s)
    print("One row per defrost cycle (defrost start to the next defrost start), %s" % s.name)
    print("start        min outdoor | err    rms   room lo..hi  | heat  power  COP | request integral | 100q 100  "
          "changes | Wh heat")
    for c in cs:
        print("%s %4.0f %6.1f  | %+.2f  %.2f  %.2f..%.2f | %4.0f  %4.0f  %s | %s    %s    | %3.0f%% %2.0f%%  %2d      "
              "| %s" % (stamp(c["t"]), c["minutes"], c["outdoor"], c["err"], c["rms"], c["lo"], c["hi"], c["heat"] or 0,
                        c["power"], "%.2f" % c["cop"] if c["cop"] else "  - ",
                        "%4.0f" % c["request"] if c["request"] is not None else "   -",
                        "%4.0f" % c["integral"] if c["integral"] is not None else "   -", 100 * c["at100q"],
                        100 * c["at100"], c["changes"], "%.0f" % c["frost_wh"] if c["frost_wh"] else "-"))
    if cs:
        iv = [c["minutes"] for c in cs]
        fw = [c["frost_wh"] for c in cs if c["frost_wh"]]
        print("\n%d cycles: %.0f min apart (median), mean |cycle error| %.2f degC%s" % (
            len(cs), st.median(iv), sum(abs(c["err"]) for c in cs) / len(cs),
            ", %.0f Wh of heat per cycle (%.0f..%.0f)" % (st.median(fw), min(fw), max(fw)) if fw else ""))


def cmd_levels(s, args):
    series = heatlog.runs() if not args.log else [s]
    tw = series[-1].table_watts()
    order = [r[0] for r in series[-1].table]
    print("Settled power and heat by level, fan setting and 5 degC outdoor band (%s)" % (
        "all saved logs" if not args.log else s.name))
    print("level table |  fan outdoor   min | power (p10-p90)   vs table | heat   COP  comp Hz | heat per table W")
    for g in sorted(heatlog.level_stats(series), key=lambda g: (order.index(g["level"]) if g["level"] in order else 99,
                                                                g["band"], g["fan"] or 0)):
        if g["minutes"] < args.min or g["level"] not in tw:
            continue
        t = tw[g["level"]]
        print("%5s %5.0f | %4s %+6.1f  %5.0f | %4.0f (%4.0f-%4.0f)   %+4.0f%%    | %s  %s  %3.0f     | %s" % (
            g["level"], t, g["fan"], g["outdoor"], g["minutes"], g["power"], g["p10"], g["p90"],
            100 * (g["power"] / t - 1), "%4.0f" % g["heat"] if g["heat"] else "   -",
            "%.2f" % g["cop"] if g["cop"] else "  - ", g["comp"], "%.2f" % (g["heat"] / t) if g["heat"] else "-"))
    print("\nThe controller multiplies the table by ONE learnt heat-per-watt figure, so only the ratios between levels "
          "matter to it.\nThe last column shows where that breaks: levels whose figure differs are mis-predicted by "
          "that ratio.")


def cmd_steps(s, args):
    rows = step_rows(s)
    if not rows:
        print("no heat deficit in this log (older firmware): nothing to show")
        return
    print("Every demand step outside a defrost, %s" % s.name)
    print("time           step      deficit thresh | target | room-eff | next level: predicted actual      | defrost in")
    for r in rows:
        print("%s %4s>%-4s  %+5.0f   %+5.0f  | %5.0f  | %+.2f    | %5.0f     %s | %s" % (
            stamp(r["t"], True), r["from"], r["to"], r["deficit"], r["threshold"], r["target"], r["roomerr"], r["pred"],
            "%5.0f (%+3.0f%%)" % (r["actual"], 100 * (r["actual"] / r["pred"] - 1)) if r["actual"] else "    -        ",
            "%.0f min" % r["to_defrost"] if r["to_defrost"] is not None else "-"))
    print("\nThreshold is for demand.rate %.0f s (device.json, or the default). Actual is the mean estimated heat 2 "
          "to 8 min after the step, so a step up to 100%% reads low while the unit ramps." % rows[0]["rate"])


def cmd_defrost(s, args):
    k = max(1, int(round(60.0 / s.interval)))
    d = [(a, b) for a, b in s.defrosts() if a > 2 * k and b + 22 * k < s.n]
    by = {}
    for a, b in d:
        by.setdefault(s.level(b) or "?", []).append((a, b))
    for lv, sel in sorted(by.items(), key=lambda x: -len(x[1])):
        print("%d defrosts with level %s after them (%s)" % (len(sel), lv, s.name))
        print("  min after defrost end:        " + " ".join("%5d" % m for m in range(-2, 22, 2)))
        for key, label in (("power", "power W"), ("heatout", "heat W"), ("comp", "compressor Hz"),
                           ("room", "room vs defrost start"), ("demand", "demand")):
            if not s.has(key):
                continue
            row = []
            for m in range(-2, 22, 2):
                v = []
                for a, b in sel:
                    x = s.col(key)[b + m * k]
                    if x is None:
                        continue
                    v.append(x - (s.col("room")[a] or x) if key == "room" else x)
                row.append(st.median(v) if v else 0)
            print("  %-28s" % label + " ".join(("%5.2f" if key == "room" else "%5.0f") % x for x in row))
        low, drop = [], []
        for a, b in sel:
            seg = [x for x in s.col("room")[a:b + 22 * k] if x is not None]
            if seg and s.col("room")[a] is not None:
                j = s.col("room")[a:b + 22 * k].index(min(seg))
                low.append((a + j - b) / float(k))
                drop.append(min(seg) - s.col("room")[a])
        if low:
            print("  room lowest %.1f min after the defrost ends, %.2f degC below where the defrost began\n" % (
                st.median(low), st.median(drop)))
    cs = [c for c in heatlog.cycle_stats(s) if c["frost_wh"]]
    w = weather.load()
    for c in cs:  # Humidity and wind at the weather station through the cycle, None where not on record
        c["rh"], c["wind"] = (w.mean(k, c["t"], c["t"] + c["minutes"] * 60) if w else None for k in ("rh", "wind"))
    if cs:
        print("Heat given per cycle against the cycle's length and outdoor temperature (does frost follow heat?):")
        for c in cs:
            print("  %s  %3.0f min  outdoor %+5.1f  %5.0f Wh  mean %4.0f W  at 100q %2.0f%%%s" % (
                stamp(c["t"]), c["minutes"], c["outdoor"], c["frost_wh"], c["heat"], 100 * c["at100q"],
                "  humidity %3.0f%%  wind %4.1f m/s" % (c["rh"], c["wind"]) if None not in (c["rh"], c["wind"]) else ""))
        if not w:
            print("  (no humidity or wind: weather.py has nothing on record)")
        normal = [c for c in cs if c["minutes"] >= heatlog.SHORT_CYCLE]
        if len(normal) >= 3:
            iv, fw = [c["minutes"] for c in normal], [c["frost_wh"] for c in normal]
            print("  spread over the %d cycles of %d min or more: length %.0f%%, heat per cycle %.0f%% (relative "
                  "standard deviation; the smaller one is the better rule)" % (
                      len(normal), heatlog.SHORT_CYCLE, 100 * st.pstdev(iv) / st.mean(iv),
                      100 * st.pstdev(fw) / st.mean(fw)))
        loops(s, cs)


def loops(s, cs):
    """Short cycles (a defrost soon after the last one) against normal ones: where and when they come"""
    short = [c for c in cs if c["minutes"] < heatlog.SHORT_CYCLE]
    if not short:
        print("\nNo short cycles (a defrost within %d min of the last)." % heatlog.SHORT_CYCLE)
        return
    normal = [c for c in cs if c["minutes"] >= heatlog.SHORT_CYCLE]

    def cop(v):
        p = sum(c["power"] * c["minutes"] for c in v)
        return sum(c["heat"] * c["minutes"] for c in v) / p if p else 0
    print("\nShort cycles (under %d min from one defrost to the next): %d of %d, %.0f%% of the time" % (
        heatlog.SHORT_CYCLE, len(short), len(cs), 100 * sum(c["minutes"] for c in short) / sum(c["minutes"] for c in cs)))
    print("  COP with the defrost counted in: short %.2f, normal %.2f; mean heat %.0f W against %.0f W" % (
        cop(short), cop(normal) if normal else 0, st.mean(c["heat"] for c in short),
        st.mean(c["heat"] for c in normal) if normal else 0))
    after = [(cs[i - 1]["minutes"] < heatlog.SHORT_CYCLE, cs[i]["minutes"] < heatlog.SHORT_CYCLE)
             for i in range(1, len(cs)) if abs(cs[i]["t"] - cs[i - 1]["t"] - cs[i - 1]["minutes"] * 60) < 120]
    for was, label in ((True, "a short"), (False, "a normal")):
        v = [b for a, b in after if a == was]
        if v:
            print("  after %s cycle the next is short in %d of %d" % (label, sum(v), len(v)))
    print("  by outdoor temperature:      " + "  ".join("%+d..%+d: %d short, %d normal" % (
        b, b + 2, sum(1 for c in short if b <= c["outdoor"] < b + 2), sum(1 for c in normal if b <= c["outdoor"] < b + 2))
        for b in range(-12, 12, 2) if any(b <= c["outdoor"] < b + 2 for c in cs)))
    wet = [c for c in cs if c.get("rh") is not None]
    if wet:
        st_ = (weather.load().stations or {}).get("weather") or {}
        print("  by humidity at %s (%s km away): " % (st_.get("name", "the weather station"), st_.get("km", "?")) + "  ".join(
            "%d-%d%%: %d short, %d normal" % (lo, min(hi, 100), sum(1 for c in wet if lo <= c["rh"] < hi and c in short),
                                             sum(1 for c in wet if lo <= c["rh"] < hi and c in normal))
            for lo, hi in ((0, 80), (80, 90), (90, 95), (95, 101)) if any(lo <= c["rh"] < hi for c in wet)))
    # Load just after the defrost against whether the cycle turned out short
    k = lambda sec: int(sec / s.interval)
    rows_ = []
    for c in cs:
        d = next(((x, y) for x, y in s.defrosts() if x == c["a"]), None)
        if d and min(d[1] + k(1080), c["b"]) - (d[1] + k(180)) >= k(300):
            rows_.append((c["minutes"] < heatlog.SHORT_CYCLE, s.mean("power", d[1] + k(180), min(d[1] + k(1080), c["b"])),
                          c["outdoor"]))
    print("  by mean power 3 to 18 min after the defrost: " + "  ".join(
        "%d-%d W: %d short of %d" % (lo, hi, sum(r[0] for r in v), len(v)) for lo, hi, v in (
            (lo, hi, [r for r in rows_ if r[1] is not None and lo <= r[1] < hi])
            for lo, hi in ((0, 550), (550, 750), (750, 4000))) if v))
    # What the unit was doing in the cycle before each short one, against before each normal one
    rows = {}
    for i in range(1, len(cs)):
        prev, kind = cs[i - 1], "short" if cs[i]["minutes"] < heatlog.SHORT_CYCLE else "normal"
        a, b = prev["a"], prev["b"]
        # The defrost that ended the previous cycle is the one at cs[i]["a"]: its length, and the level after it
        d = next(((x, y) for x, y in s.defrosts() if x == cs[i]["a"]), None)
        if not d:
            continue
        r = rows.setdefault(kind, {"defrost_s": [], "level": {}, "prev_heat": [], "liquid": []})
        r["defrost_s"].append((d[1] - d[0]) * s.interval)
        lv = s.level(min(d[1] + int(300 / s.interval), s.n - 1)) or "-"
        r["level"][lv] = r["level"].get(lv, 0) + 1
        r["prev_heat"].append(prev["frost_wh"])
        liq = [x for x in s.col("liquid")[d[0]:d[1]] if x is not None]
        if liq:
            r["liquid"].append(min(liq))
    for kind in ("short", "normal"):
        r = rows.get(kind)
        if r:
            print("  cycles that turned out %-6s: the defrost at their start took %.0f s (median), liquid down to "
                  "%.0f degC, after %.0f Wh of heat; level 5 min into them: %s" % (
                      kind, st.median(r["defrost_s"]), st.median(r["liquid"]) if r["liquid"] else 0,
                      st.median(r["prev_heat"]), ", ".join("%s x%d" % kv for kv in sorted(
                          r["level"].items(), key=lambda kv: -kv[1])[:6])))


def parse_when(text, s):
    year = datetime.datetime.fromtimestamp(s.t[-1]).year
    end = datetime.datetime.fromtimestamp(s.t[-1])
    for full in (text, "%d-%s" % (year, text), "%s %s" % (end.strftime("%Y-%m-%d"), text)):
        try:
            return datetime.datetime.strptime(full, "%Y-%m-%d %H:%M").timestamp()
        except ValueError:
            continue
    raise SystemExit("time not understood: %s (use \"MM-DD HH:MM\")" % text)


def cmd_trace(s, args):
    a = s.index_at(parse_when(args.frm, s)) if args.frm else max(0, s.n - 240)
    b = s.index_at(parse_when(args.to, s)) if args.to else s.n
    print("time           level sub   room   eff    P    integ  req   tgt  defic | power  heat  liquid comp fan")
    for i in range(a, b, args.every):
        g = s.col
        e = (g("effective")[i] - g("room")[i]) if g("room")[i] is not None and g("effective")[i] is not None else None
        kp = g("kp")[i]

        def f(key, fmt="%5.0f"):
            return fmt % g(key)[i] if g(key)[i] is not None else " " * (len(fmt % 0) - 1) + "-"
        print("%s %5s %-5s %s %s %s %s %s %s %s | %s %s  %s %s %s" % (
            stamp(s.t[i], True), s.level(i) or "-", (s.subs[g("sub")[i]] if g("sub")[i] else "")[:5],
            f("room", "%.2f"), f("effective", "%.2f"), "%+5.0f" % (e * kp) if e is not None and kp else "    -",
            f("integral"), f("heatw"), f("heattarget"), f("deficit"), f("power"), f("heatout"), f("liquid", "%5.1f"),
            f("comp", "%3.0f"), f("fan", "%4.0f")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["check", "cycles", "levels", "steps", "defrost", "trace"])
    ap.add_argument("log", nargs="?")
    ap.add_argument("--from", dest="frm")
    ap.add_argument("--to")
    ap.add_argument("--every", type=int, default=8, help="trace: every Nth sample")
    ap.add_argument("--min", type=float, default=5, help="levels: least minutes for a row")
    args = ap.parse_args()
    s = heatlog.find_log(args.log)
    {"check": cmd_check, "cycles": cmd_cycles, "levels": cmd_levels, "steps": cmd_steps, "defrost": cmd_defrost,
     "trace": cmd_trace}[args.command](s, args)


if __name__ == "__main__":
    main()
