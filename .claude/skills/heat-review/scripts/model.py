#!/usr/bin/env python3
"""The local model of the house and the unit, kept in graph-logs/model.json and fitted from the saved logs.

Usage: python3 -I model.py show | validate [log] | fit [--dry-run] [--hours 240] | holdout | residual [log]

  show      the model as it stands
  validate  test the model AS IT STANDS against a log (default the newest), before any refit: rolling forecasts
            of the room against "no model", and the unit and defrost figures against what the log shows
  fit       refit everything from all saved logs, print old -> new, and save (not with --dry-run)
  holdout   fit the house on the earlier part of the longest stretch and score it on the part held back
  residual  heat the room got that the model does not account for, every two hours along a log: where to look
            for a heat source or a loss nobody has mentioned yet

House: two nodes. The room air (air_J_K) takes the unit's heat and exchanges with a mass (air_mass_W_K, mass_J_K)
that loses to outdoors (mass_out_W_K). On top of the unit's heat the room gets: what a defrost takes out
(defrost.room_W), a gain by time of day relative to the night (gains.daily_W), and heat sources the user has told
of, listed in graph-logs/events.json (gains.sources_W, per source and hours since its start).
Unit: heat and power per watt of the demand table up to 95%, and the two 100% levels, each as a + b * outdoor.
Defrost: one comes once frost_Wh of heat has been given since the last, and lasts `seconds`.
See references/model.md for what this can and cannot tell.
"""
import argparse
import math
import os
import random
import statistics as st
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402
from heatlog import stamp  # noqa: E402

DT = 60.0
START = {  # Round figures to start the first fit from; the fit replaces them all
    "house": {"air_J_K": 4.0e6, "air_mass_W_K": 1000.0, "mass_J_K": 5.0e7, "mass_out_W_K": 90.0},
    "defrost": {"room_W": -2500.0, "seconds": 500.0, "lead_s": 60.0, "frost_Wh": 3000.0},
    "unit": {"heat_per_table": {"a": 3.5, "b": 0.0}, "power_per_table": 1.0,
             "q100q": {"a": 5000.0, "b": 0.0}, "p100q": {"a": 1500.0, "b": 0.0},
             "q100": {"a": 7000.0, "b": 0.0}, "p100": {"a": 2500.0, "b": 0.0},
             "ramp_low_W_min": 1200.0, "ramp_high_W_min": 350.0},
    "gains": {},
}
BOUNDS = {"air_J_K": (1e6, 1.2e7), "air_mass_W_K": (100, 3000), "mass_J_K": (5e6, 3e8), "mass_out_W_K": (30, 400)}
HOUSE = ("air_J_K", "air_mass_W_K", "mass_J_K", "mass_out_W_K")
DAY_BINS = ((6, 12), (12, 18), (18, 24))  # Gain by time of day, against 00-06 as zero
SOURCE_BINS = ((0, 2), (2, 4), (4, 7))    # A told-of heat source, by hours since its start
LEAD, AHEAD = 240, 720                    # Rolling forecasts: 4 h to set the mass temperature, then 12 h unaided
HORIZONS = (60, 180, 360, 720)


def model_path():
    return os.path.join(heatlog.logdir(), "model.json")


def events_path():
    return os.path.join(heatlog.logdir(), "events.json")


def load_model():
    m = heatlog.read_json(model_path())
    if not m:
        m = {k: dict(v) for k, v in START.items()}
        m["unit"] = {k: (dict(v) if isinstance(v, dict) else v) for k, v in START["unit"].items()}
    m.setdefault("gains", {})
    return m


def load_sources():
    """{name: [start epoch, ...]} from events.json: heat sources the user has told of"""
    out = {}
    for e in (heatlog.read_json(events_path(), {}) or {}).get("sources", []):
        try:
            out.setdefault(e["name"], []).append(time.mktime(time.strptime(e["start"], "%Y-%m-%d %H:%M")))
        except (KeyError, ValueError):
            print("events.json: entry not understood, skipped: %s" % e)
    return out


def ua(h):
    return h["air_mass_W_K"] * h["mass_out_W_K"] / (h["air_mass_W_K"] + h["mass_out_W_K"])


# --- data: one-minute bins of room, heat (in all and by fan setting), defrost share and a cleaned outdoor reading


def segments(series_list, since=None):
    out = []
    for s in series_list:
        if not s.has("heatout"):
            continue
        k = max(1, int(round(DT / s.interval)))
        sub, od, fan, heatout = s.col("sub"), list(s.col("outdoor")), s.col("fanset"), s.col("heatout")
        # The unit's outdoor reading jumps in a defrost: drop it then and for 15 min after, and smooth over an hour
        bad = [False] * s.n
        for a, b in s.defrosts():
            for j in range(a, min(s.n, b + int(900 / s.interval))):
                bad[j] = True
        last = next((x for x, f in zip(od, bad) if x is not None and not f), None)
        if last is None:
            continue
        for i in range(s.n):
            if od[i] is None or bad[i]:
                od[i] = last
            else:
                last = od[i]
        cur = None
        for i in range(0, s.n - k + 1, k):
            room = [x for x in s.col("room")[i:i + k] if x is not None]
            heat = [x for x in heatout[i:i + k] if x is not None]
            gap = s.t[i + k - 1] - s.t[i] > 2 * DT
            if not room or not heat or gap or (since and s.t[i] < since):
                cur = None
                continue
            if cur is None:
                cur = {"name": s.name, "t": [], "R": [], "Q": [], "D": [], "O": [], "F": {}}
                out.append(cur)
            d = sum(1 for x in sub[i:i + k] if x == heatlog.DEFROST) / float(k)
            w = int(1800 / s.interval)
            n = len(cur["R"])
            cur["t"].append(s.t[i])
            cur["R"].append(sum(room) / len(room))
            cur["Q"].append(sum(heat) / len(heat) * (1 - d))
            cur["D"].append(d)
            cur["O"].append(st.median(od[max(0, i - w):i + w + 1]))
            for j in range(i, i + k):  # Heat by fan setting, to let the fit scale the estimate per fan speed
                if fan[j] is not None and heatout[j] is not None and sub[j] != heatlog.DEFROST:
                    cur["F"].setdefault(fan[j], {})[n] = cur["F"].setdefault(fan[j], {}).get(n, 0.0) + heatout[j] / k
    return [g for g in out if len(g["R"]) >= 180]


def cut(g, a, b):
    """Minutes a..b of a segment"""
    out = {key: (v[a:b] if isinstance(v, list) else v) for key, v in g.items()}
    out["F"] = {f: {i - a: x for i, x in d.items() if a <= i < b} for f, d in g["F"].items()}
    return out


def defrost_windows(segs, before=15, after=60):
    """Each defrost as a step test: from `before` minutes ahead of it to `after` minutes past its end"""
    out = []
    for g in segs:
        n, k = len(g["R"]), 0
        while k < n:
            if g["D"][k] > 0 and (k == 0 or g["D"][k - 1] == 0):
                e = k
                while e < n and g["D"][e] > 0:
                    e += 1
                if k - before >= 0 and e + after <= n:
                    out.append(cut(g, k - before, e + after))
                k = e
            else:
                k += 1
    return out


def rolling_windows(segs, stride=120):
    """From every `stride` minutes: LEAD minutes before, and up to AHEAD minutes after"""
    out = []
    for g in segs:
        n = len(g["R"])
        for k0 in range(LEAD, n - 60, stride):
            out.append(cut(g, k0 - LEAD, min(n, k0 + AHEAD)))
    return out


# --- channels: heat into the room per unit of an amplitude that the fit finds by least squares


def ch_defrost(g):
    return g["D"]


def ch_fan(fan):
    def f(g):
        d = g["F"].get(fan, {})
        return [d.get(i, 0.0) for i in range(len(g["R"]))]
    return f


def ch_day(lo, hi):
    def f(g):
        return [1.0 if lo <= time.localtime(t).tm_hour < hi else 0.0 for t in g["t"]]
    return f


def ch_source(starts, lo, hi):
    def f(g):
        return [1.0 if any(s + lo * 3600 <= t < s + hi * 3600 for s in starts) else 0.0 for t in g["t"]]
    return f


def fast_channels(segs):
    """The defrost, and a scale on the heat estimate for each fan setting but the commonest"""
    total = {}
    for g in segs:
        for fan, d in g["F"].items():
            total[fan] = total.get(fan, 0) + len(d)
    ref = max(total, key=total.get) if total else None
    return [("defrost", ch_defrost)] + [("fan %s" % f, ch_fan(f)) for f in sorted(total) if f != ref and total[f] >= 600], ref


def slow_channels(sources):
    ch = [("gain %02d-%02d" % b, ch_day(*b)) for b in DAY_BINS]
    for name, starts in sorted(sources.items()):
        ch += [("%s %d-%d h" % (name, lo, hi), ch_source(starts, lo, hi)) for lo, hi in SOURCE_BINS]
    return ch


def model_channels(m, segs):
    """Every channel the stored model has an amplitude for, with those amplitudes"""
    g = m.get("gains", {})
    amp, chans = {"defrost": m["defrost"]["room_W"]}, [("defrost", ch_defrost)]
    for fan, a in (g.get("heat_scale_by_fan") or {}).items():
        chans.append(("fan %s" % fan, ch_fan(int(fan))))
        amp["fan %s" % fan] = a
    for b in DAY_BINS:
        name = "%02d-%02d" % b
        if name in (g.get("daily_W") or {}):
            chans.append(("gain " + name, ch_day(*b)))
            amp["gain " + name] = g["daily_W"][name]
    sources = load_sources()
    for name, amps in sorted((g.get("sources_W") or {}).items()):
        for (lo, hi), a in zip(SOURCE_BINS, amps):
            chans.append(("%s %d-%d h" % (name, lo, hi), ch_source(sources.get(name, []), lo, hi)))
            amp["%s %d-%d h" % (name, lo, hi)] = a
    return chans, amp


# --- the house


def response(h, n, ta0, tm0, Q=None, O=None, keep_mass=False):
    """Room over n minutes from the given start, with heat Q (W) and outdoor O (or neither)"""
    ca, kam, cm, kmo = h["air_J_K"], h["air_mass_W_K"], h["mass_J_K"], h["mass_out_W_K"]
    ta, tm, out, mass = ta0, tm0, [], []
    for k in range(n):
        out.append(ta)
        mass.append(tm)
        o = O[k] if O else 0.0
        dta = (kam * (tm - ta) + (Q[k] if Q else 0.0)) / ca
        tm += (kam * (ta - tm) + kmo * (o - tm)) / cm * DT
        ta += dta * DT
    return (out, mass) if keep_mass else out


def gauss(A, b):
    n = len(b)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(M[r][i]))
        M[i], M[p] = M[p], M[i]
        if abs(M[i][i]) < 1e-12:
            M[i][i] = 1e-12
        for r in range(n):
            if r != i and M[r][i]:
                f = M[r][i] / M[i][i]
                M[r] = [x - f * y for x, y in zip(M[r], M[i])]
    return [M[i][n] / M[i][i] for i in range(n)]


def solve(h, segs, chans, fixed=None, upto=None):
    """Least squares for everything the model is linear in: the amplitude of each channel (shared by all segments,
    unless given in `fixed`) and the mass temperature at the start of each segment. With `upto`, only the first
    that many minutes of each segment are fitted. Returns (sum of squares, n, {amplitude}, [mass start], [room])."""
    fixed = fixed or {}
    free = [c for c in chans if c[0] not in fixed]
    J = len(free)
    B = response(h, max(len(g["R"]) for g in segs), 0.0, 1.0)
    AtA, Atb, parts = [[0.0] * J for _ in range(J)], [0.0] * J, []
    for g in segs:
        n = len(g["R"])
        m = min(n, upto) if upto else n
        Q = list(g["Q"])
        for name, fn in chans:
            if name in fixed and fixed[name]:
                Q = [q + fixed[name] * x for q, x in zip(Q, fn(g))]
        A = response(h, n, g["R"][0], 0.0, Q=Q, O=g["O"])
        y = [g["R"][k] - A[k] for k in range(n)]
        bb = sum(B[k] * B[k] for k in range(m)) or 1e-12
        yb = sum(y[k] * B[k] for k in range(m)) / bb
        C = [response(h, n, 0.0, 0.0, Q=fn(g)) for _, fn in free]
        cb = [sum(c[k] * B[k] for k in range(m)) / bb for c in C]
        yp = [y[k] - yb * B[k] for k in range(m)]
        Cp = [[c[k] - cb[j] * B[k] for k in range(m)] for j, c in enumerate(C)]
        for i in range(J):
            Atb[i] += sum(Cp[i][k] * yp[k] for k in range(m))
            for j in range(i, J):
                v = sum(Cp[i][k] * Cp[j][k] for k in range(m))
                AtA[i][j] += v
                if j != i:
                    AtA[j][i] += v
        parts.append((g, A, C, yb, cb, m))
    for i in range(J):  # A channel with nothing in this data stays at zero
        if AtA[i][i] < 1e-9:
            AtA[i][i] = 1.0
    amp = gauss(AtA, Atb) if J else []
    ss, cnt, tm0s, preds = 0.0, 0, [], []
    for g, A, C, yb, cb, m in parts:
        tm0 = yb - sum(a * c for a, c in zip(amp, cb))
        pred = [A[k] + tm0 * B[k] + sum(a * c[k] for a, c in zip(amp, C)) for k in range(len(A))]
        ss += sum((g["R"][k] - pred[k]) ** 2 for k in range(m))
        cnt += m
        tm0s.append(tm0)
        preds.append(pred)
    out = dict(fixed)
    out.update({name: a for (name, _), a in zip(free, amp)})
    return ss, cnt, out, tm0s, preds


def rolling(h, wins, chans, amp):
    """Forecasts from each window's LEAD on, the mass temperature set from the minutes before it. Returns
    ({horizon: (rms, mean, n)}, sum of squares, count) of forecast minus measured"""
    acc = {z: [0.0, 0.0, 0] for z in HORIZONS}
    ss = cnt = 0
    for g in wins:
        p = solve(h, [g], chans, fixed=amp, upto=LEAD)[4][0]
        for k in range(LEAD, len(p)):
            e = p[k] - g["R"][k]
            ss += e * e
            cnt += 1
        for z in HORIZONS:
            k = LEAD + z - 1
            if k < len(p):
                e = p[k] - g["R"][k]
                acc[z][0] += e * e
                acc[z][1] += e
                acc[z][2] += 1
    return ({z: (math.sqrt(v[0] / v[2]), v[1] / v[2], v[2]) if v[2] else (0.0, 0.0, 0) for z, v in acc.items()},
            ss, cnt)


def no_model(wins):
    """The same forecasts by "the room stays as it was": what a model has to beat"""
    out = {}
    for z in HORIZONS:
        e = [g["R"][LEAD - 1] - g["R"][LEAD + z - 1] for g in wins if LEAD + z - 1 < len(g["R"])]
        out[z] = math.sqrt(sum(x * x for x in e) / len(e)) if e else 0.0
    return out


def score_lines(label, score, base=None):
    line = "  %-40s " % label + "  ".join("%2d h: %.2f (%+.2f)" % (z // 60, score[z][0], score[z][1]) for z in HORIZONS)
    if base:
        line += "\n  %-40s " % "  no model, the same forecasts" + "  ".join(
            "%2d h: %.2f        " % (z // 60, base[z]) for z in HORIZONS)
    return line


def nelder_mead(f, x0, step, iters):
    m = len(x0)
    S = [list(x0)] + [[x0[j] + (step[j] if j == i else 0) for j in range(m)] for i in range(m)]
    F = [f(x) for x in S]
    for _ in range(iters):
        o = sorted(range(m + 1), key=lambda i: F[i])
        S, F = [S[i] for i in o], [F[i] for i in o]
        if F[-1] - F[0] < 1e-9:
            break
        c = [sum(S[i][j] for i in range(m)) / m for j in range(m)]
        xr = [2 * c[j] - S[-1][j] for j in range(m)]
        fr = f(xr)
        if fr < F[0]:
            xe = [3 * c[j] - 2 * S[-1][j] for j in range(m)]
            fe = f(xe)
            S[-1], F[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < F[-2]:
            S[-1], F[-1] = xr, fr
        else:
            xc = [(c[j] + S[-1][j]) / 2 for j in range(m)]
            fc = f(xc)
            if fc < F[-1]:
                S[-1], F[-1] = xc, fc
            else:
                for i in range(1, m + 1):
                    S[i] = [(S[0][j] + S[i][j]) / 2 for j in range(m)]
                    F[i] = f(S[i])
    i = min(range(m + 1), key=lambda i: F[i])
    return S[i], F[i]


def fit_house(segs, start, sources, quiet=False):
    """Two stages, each fitted on what it is for.

    Fast (air, air to mass; with them the heat a defrost takes and a scale on the heat estimate per fan setting):
    on the defrosts as step tests, each window with its own mass temperature. This sets the room's swing in a
    defrost cycle, which is what the controller reacts to.
    Slow (mass, mass to outdoors; with them the gain by time of day and the told-of heat sources): on rolling
    forecasts, the mass temperature set from four hours and the room then forecast twelve hours on. Scoring a
    forecast and not one long run keeps heat nobody logged from being soaked up by the mass.
    """
    def say(text):
        if not quiet:
            print(text, flush=True)

    def run(keys, base, cost, iters, tries, label):
        random.seed(1)
        best = None
        for t in range(tries):
            x0 = [math.log(base[k]) + (random.uniform(-0.4, 0.4) if t else 0) for k in keys]

            def c(x):
                h = dict(base)
                h.update({k: math.exp(v) for k, v in zip(keys, x)})
                if any(not BOUNDS[k][0] <= h[k] <= BOUNDS[k][1] for k in keys):
                    return 1e9
                return cost(h)
            x, fx = nelder_mead(c, x0, [0.3] * len(keys), iters)
            say("  %s, start %d: rms %.3f degC  %s" % (label, t + 1, math.sqrt(fx), ", ".join(
                "%s %.4g" % (k, math.exp(v)) for k, v in zip(keys, x))))
            if best is None or fx < best[0]:
                best = (fx, x)
        h = dict(base)
        h.update({k: math.exp(v) for k, v in zip(keys, best[1])})
        return h, math.sqrt(best[0])

    h, info = dict(start), {}
    dw = defrost_windows(segs)
    fchans, ref = fast_channels(dw)
    amp = {"defrost": None}
    if len(dw) >= 8:
        h, info["rms_through_defrosts"] = run(("air_J_K", "air_mass_W_K"), h, lambda hh: (
            lambda r: r[0] / r[1])(solve(hh, dw, fchans)), 150, 3, "fast, %d defrosts" % len(dw))
        amp = solve(h, dw, fchans)[2]
    else:
        say("  fast: only %d defrosts with room around them, air figures kept" % len(dw))
        fchans, amp = [("defrost", ch_defrost)], {}
    schans = slow_channels(sources)
    rw = rolling_windows(segs, stride=180)

    def amps(hh):
        return solve(hh, rw, fchans + schans, fixed=amp)[2]

    def cost(hh):
        _, ss, cnt = rolling(hh, rw, fchans + schans, amps(hh))
        return ss / cnt
    if len(rw) >= 6:
        h, info["rms_forecast"] = run(("mass_J_K", "mass_out_W_K"), h, cost, 60, 2, "slow, %d forecasts" % len(rw))
        amp = amps(h)
    else:
        say("  slow: under a day of data, mass figures kept")
        if amp.get("defrost") is None:
            amp["defrost"] = None
    info["reference_fan"] = ref
    return h, amp, fchans + schans, info


def gains_from(amp, sources, ref):
    g = {"daily_W": {}, "sources_W": {}, "heat_scale_by_fan": {}, "reference_fan": ref}
    for b in DAY_BINS:
        name = "gain %02d-%02d" % b
        if name in amp:
            g["daily_W"]["%02d-%02d" % b] = amp[name]
    for name in sources:
        bins = ["%s %d-%d h" % (name, lo, hi) for lo, hi in SOURCE_BINS]
        if all(b in amp for b in bins):
            g["sources_W"][name] = [amp[b] for b in bins]
    for k, v in amp.items():
        if k.startswith("fan "):
            g["heat_scale_by_fan"][k[4:]] = v
    return g


def unexplained(h, chans, amp, segs, step_h=2):
    """Heat the room got beyond what the model accounts for (W), per `step_h` hours along the data: each 20 minutes
    run from the measured room, against the measured room at their end. Defrosts and the 15 min after are left out."""
    rows = []
    _, _, _, tm0s, _ = solve(h, segs, chans, fixed=amp)
    for g, tm0 in zip(segs, tm0s):
        n = len(g["R"])
        Q = list(g["Q"])
        for name, fn in chans:
            if amp.get(name):
                Q = [q + amp[name] * x for q, x in zip(Q, fn(g))]
        _, mass = response(h, n, g["R"][0], tm0, Q=Q, O=g["O"], keep_mass=True)
        bins = {}
        for k in range(0, n - 20, 10):
            if any(g["D"][k:k + 20]) or any(g["D"][max(0, k - 15):k]):
                continue
            ta = response(h, 21, g["R"][k], mass[k], Q=Q[k:k + 21], O=g["O"][k:k + 21])[20]
            bins.setdefault(int(g["t"][k] // (step_h * 3600)), []).append(((g["R"][k + 20] - ta) * h["air_J_K"] / 1200.0, k))
        for b, v in sorted(bins.items()):
            ks = [x[1] for x in v]
            a, e = min(ks), max(ks) + 20
            rows.append({"t": b * step_h * 3600, "W": sum(x[0] for x in v) / len(v), "n": len(v),
                         "room": sum(g["R"][a:e]) / (e - a), "outdoor": sum(g["O"][a:e]) / (e - a),
                         "heat": sum(g["Q"][a:e]) / (e - a)})
    return rows


# --- the unit and the defrosts, read off the logs


def line_fit(points, old):
    """points: [(outdoor, value, weight)] -> {"a": value at 0 degC, "b": per degC}; the slope is only refitted
    when the points span 4 degC, else the old slope is kept and only the level moves"""
    if not points:
        return dict(old)
    W = sum(p[2] for p in points)
    mx = sum(p[0] * p[2] for p in points) / W
    my = sum(p[1] * p[2] for p in points) / W
    sxx = sum(p[2] * (p[0] - mx) ** 2 for p in points)
    span = max(p[0] for p in points) - min(p[0] for p in points)
    b = sum(p[2] * (p[0] - mx) * (p[1] - my) for p in points) / sxx if span >= 4 and sxx > 0 else old.get("b", 0.0)
    return {"a": my - b * mx, "b": b, "points": len(points), "span": [min(p[0] for p in points),
                                                                    max(p[0] for p in points)]}


def at(line, outdoor):
    """The line at an outdoor temperature, held flat beyond 3 degC outside the data it was fitted on"""
    span = line.get("span")
    if span:
        outdoor = max(span[0] - 3, min(span[1] + 3, outdoor))
    return line["a"] + line.get("b", 0.0) * outdoor


def unit_table(unit):
    """Level name -> watts of the table the unit's figures are per watt of: fixed in the model, so that a change of
    demand.table on the device does not change what the model says the unit gives"""
    return {name: w for name, _, _, w in heatlog.parse_table(unit.get("table") or heatlog.DEFAULT_TABLE)}


def fit_unit(series_list, old):
    groups = [g for g in heatlog.level_stats(series_list) if g["minutes"] >= 10]
    tw = unit_table(old)
    low = [g for g in groups if g["level"] in tw and not g["level"].startswith("100")]
    u = {k: (dict(v) if isinstance(v, dict) else v) for k, v in old.items()}
    u["heat_per_table"] = line_fit([(g["outdoor"], g["heat"] / tw[g["level"]], g["minutes"]) for g in low
                                    if g["heat"]], old["heat_per_table"])
    pw = [(g["power"] / tw[g["level"]], g["minutes"]) for g in low]
    if pw:
        u["power_per_table"] = sum(a * b for a, b in pw) / sum(b for _, b in pw)
    for lv, qk, pk in (("100q", "q100q", "p100q"), ("100", "q100", "p100")):
        sel = [g for g in groups if g["level"] == lv]
        u[qk] = line_fit([(g["outdoor"], g["heat"], g["minutes"]) for g in sel if g["heat"]], old[qk])
        u[pk] = line_fit([(g["outdoor"], g["power"], g["minutes"]) for g in sel], old[pk])
    # Ramp to 100q: from the step to 80% of the way to the settled heat
    rates = []
    for s in series_list:
        if not s.has("heatout"):
            continue
        out = s.col("heatout")
        for i in range(1, s.n - int(900 / s.interval)):
            if s.level(i) == "100q" and s.level(i - 1) == "95" and s.col("sub")[i] == 0 and out[i] is not None:
                top = [x for x in out[i + int(480 / s.interval):i + int(720 / s.interval)] if x is not None]
                if not top or s.level(i + int(700 / s.interval)) != "100q":
                    continue
                goal = out[i] + 0.8 * (st.median(top) - out[i])
                for j in range(i, i + int(720 / s.interval)):
                    if out[j] is not None and out[j] >= goal and j > i:
                        rates.append((goal - out[i]) / ((j - i) * s.interval / 60.0))
                        break
    if len(rates) >= 5:
        u["ramp_high_W_min"] = st.median(rates)
    u["table"] = old.get("table") or heatlog.DEFAULT_TABLE
    return u


def fit_defrost(series_list, old):
    d = dict(old)
    dur, lead, frost, short = [], [], [], []
    for s in series_list:
        if not s.has("heatout"):
            continue
        out = s.col("heatout")
        for a, b in s.defrosts():
            if b < s.n and a > int(900 / s.interval):
                dur.append((b - a) * s.interval)
                before = [x for x in out[a - int(900 / s.interval):a - int(300 / s.interval)] if x is not None]
                if before:
                    normal, j = st.median(before), a - 1
                    while j > 0 and out[j] is not None and out[j] < 0.5 * normal:
                        j -= 1
                    lead.append((a - 1 - j) * s.interval)
        cs = [c for c in heatlog.cycle_stats(s) if c["frost_wh"] and c["demand_time"] > 0.9]
        # Short cycles (defrost loops) are not what the frost rule describes: counted, and left out of it
        frost += [c["frost_wh"] for c in cs if c["minutes"] >= heatlog.SHORT_CYCLE]
        short += [c["frost_wh"] for c in cs if c["minutes"] < heatlog.SHORT_CYCLE]
    if dur:
        d["seconds"] = st.median(dur)
    if lead:
        d["lead_s"] = st.median(lead)
    if len(frost) >= 4:
        d.update({"frost_Wh": st.median(frost), "frost_Wh_p10": heatlog.pct(frost, 0.1),
                  "frost_Wh_p90": heatlog.pct(frost, 0.9), "cycles": len(frost)})
    d["short_cycles"] = len(short)
    d["short_share"] = len(short) / float(len(short) + len(frost)) if short or frost else 0.0
    if short:
        d["short_Wh"] = st.median(short)
    return d




def show(m, title="Model"):
    h, d, u, f, g = m["house"], m["defrost"], m["unit"], m.get("fit") or {}, m.get("gains") or {}
    print("%s%s" % (title, " (fitted %s on %.0f h)" % (f["date"], f["hours"]) if f else
                    " (starting values, never fitted)"))
    print("  house   air %.2f MJ/K, air to mass %.0f W/K, mass %.0f MJ/K, mass to outdoors %.0f W/K" % (
        h["air_J_K"] / 1e6, h["air_mass_W_K"], h["mass_J_K"] / 1e6, h["mass_out_W_K"]))
    print("          overall loss %.0f W/K, fast time constant %.0f min, slow %.0f h" % (
        ua(h), h["air_J_K"] / h["air_mass_W_K"] / 60, h["mass_J_K"] / (h["mass_out_W_K"]) / 3600))
    if f.get("rms_through_defrosts"):
        print("          through the defrosts (15 min before to 60 min after each) the room is followed to %.3f degC "
              "rms" % f["rms_through_defrosts"])
    if g.get("daily_W"):
        print("  gains   by time of day against 00-06: " + ", ".join("%s %+.0f W" % kv for kv in sorted(g["daily_W"].items())))
    for name, a in sorted((g.get("sources_W") or {}).items()):
        print("          %s: " % name + ", ".join("%d-%d h %+.0f W" % (lo, hi, x) for (lo, hi), x in zip(SOURCE_BINS, a)))
    if g.get("heat_scale_by_fan"):
        print("          heat estimate against fan %s: " % g.get("reference_fan") + ", ".join(
            "fan %s %+.0f%%" % (k, 100 * v) for k, v in sorted(g["heat_scale_by_fan"].items())))
    print("  defrost every %.0f Wh of heat%s, %.0f s long, takes %.0f W from the room, output falls %.0f s before" % (
        d["frost_Wh"], " (%.0f..%.0f over %d cycles)" % (d["frost_Wh_p10"], d["frost_Wh_p90"], d["cycles"])
        if "cycles" in d else "", d["seconds"], -d["room_W"], d["lead_s"]))
    if d.get("short_cycles"):
        print("          NOT MODELLED: %.0f%% of the cycles in the logs were short ones (a defrost again after about "
              "%.0f Wh): the plant has no defrost loops" % (100 * d["short_share"], d.get("short_Wh", 0)))
    for T in (-10, -5, 0, 5):
        print("  unit at %+3d degC: %.2f W heat and %.2f W power per table W up to 95%%; 100q %.0f W heat, %.0f W; "
              "100 %.0f W heat, %.0f W" % (T, at(u["heat_per_table"], T), u["power_per_table"], at(u["q100q"], T),
                                           at(u["p100q"], T), at(u["q100"], T), at(u["p100"], T)))
    print("  ramp   %.0f W/min up to the 95%% level's heat, %.0f W/min above it" % (
        u["ramp_low_W_min"], u["ramp_high_W_min"]))


def usable_series(hours=None):
    series = heatlog.runs()
    if hours:
        end = max(s.t[-1] for s in series)
        series = [s.slice(s.index_at(end - hours * 3600), s.n) for s in series if s.t[-1] > end - hours * 3600]
    return [s for s in series if s.has("heatout")]


def cmd_validate(args):
    m = load_model()
    log = heatlog.find_log(args.log)
    show(m, "Model as it stands")
    since = (m.get("fit") or {}).get("fitted_until")
    new_only = bool(since and log.t[-1] > since + 6 * 3600 and log.t[0] < since)
    # The forecasts need four hours before their start, so the new part is taken with that much of the old
    segs = segments([log], since - LEAD * 60 if new_only else None)
    chans, amp = model_channels(m, segs)
    print("\nHouse, rolling forecasts on %s%s:" % (log.name, " (only the part after the last fit, %s)" % stamp(since)
                                                  if new_only else (" (IN the last fit: not an independent test)"
                                                                    if since and log.t[-1] <= since + 6 * 3600 else "")))
    wins = rolling_windows(segs) if segs else []
    if len(wins) < 4:
        print("  not enough data with a heat estimate (4 h to set the start, then hours to forecast)")
    else:
        score, _, _ = rolling(m["house"], wins, chans, amp)
        base = no_model(wins)
        print(score_lines("model, %d forecasts: rms (mean) degC" % len(wins), score, base))
        verdict = [z // 60 for z in HORIZONS if score[z][0] < base[z]]
        print("  The model beats \"the room stays as it was\" at: %s. Positive mean: the model runs warm." % (
            ", ".join("%d h" % z for z in verdict) or "no horizon"))
        if load_sources():
            print("  Heat sources in events.json are counted where the model has figures for them; one that is new "
                  "since the fit is not.")
    print("\nUnit and defrost, model against this log:")
    cs = [c for c in heatlog.cycle_stats(log) if c["frost_wh"] and c["demand_time"] > 0.9]
    d = m["defrost"]

    def row(label, model, seen, tol, unit):
        if seen is None:
            return
        off = 100 * (seen / model - 1) if model else 0
        print("  %-34s model %7.0f %s, log %7.0f %s (%+.0f%%)  [%s]" % (label, model, unit, seen, unit, off,
                                                                         "ok" if abs(off) <= tol else "OFF"))
    short = [c for c in cs if c["minutes"] < heatlog.SHORT_CYCLE]
    cs = [c for c in cs if c["minutes"] >= heatlog.SHORT_CYCLE]
    if short:
        print("  %d of %d cycles are short ones (defrost loops), which the model does not have  [OFF]" % (
            len(short), len(short) + len(cs)))
    if cs:
        row("heat given per defrost cycle", d["frost_Wh"], st.median([c["frost_wh"] for c in cs]), 15, "Wh")
    dur = [(b - a) * log.interval for a, b in log.defrosts() if b < log.n]
    if dur:
        row("defrost length", d["seconds"], st.median(dur), 20, "s")
    tw = unit_table(m["unit"])
    for g in sorted(heatlog.level_stats([log]), key=lambda g: (tw.get(g["level"], 0), g["band"])):
        if g["minutes"] < 30 or not g["heat"] or g["level"] not in tw:
            continue
        T, u = g["outdoor"], m["unit"]
        if g["level"] == "100q":
            mh, mp = at(u["q100q"], T), at(u["p100q"], T)
        elif g["level"] == "100":
            mh, mp = at(u["q100"], T), at(u["p100"], T)
        else:
            mh, mp = at(u["heat_per_table"], T) * tw[g["level"]], u["power_per_table"] * tw[g["level"]]
        row("heat at %s, fan %s, %+.0f degC" % (g["level"], g["fan"], T), mh, g["heat"], 10, "W")
        row("power at %s, fan %s, %+.0f degC" % (g["level"], g["fan"], T), mp, g["power"], 10, "W")


def cmd_holdout(args):
    m = load_model()
    segs = segments(usable_series(args.hours))
    if not segs:
        raise SystemExit("no log has a heat output estimate")
    sources = load_sources()
    longest = max(segs, key=lambda g: len(g["R"]))
    rest = [g for g in segs if g is not longest]
    n = len(longest["R"])
    print("The house fitted on the earlier part of %s (with the other stretches) and scored on the part held back." %
          longest["name"][:60])
    print("rms (mean) of forecast minus measured, degC. A model worth having beats \"no model\" on data it has not "
          "seen.\n")
    for label, c in (("second half held back", n // 2), ("last third held back", 2 * n // 3)):
        h, amp, chans, info = fit_house(rest + [cut(longest, 0, c)], m["house"], sources, quiet=True)
        test = [cut(longest, c, n)]
        print("%s (from %s): fitted mass %.0f MJ/K, to outdoors %.0f W/K, overall %.0f W/K" % (
            label, stamp(longest["t"][c]), h["mass_J_K"] / 1e6, h["mass_out_W_K"], ua(h)))
        dw = defrost_windows(test)
        if dw:
            ss, cnt, _, _, _ = solve(h, dw, chans, fixed=amp)
            print("  %d unseen defrosts followed to %.3f degC rms (air %.2f MJ/K, air to mass %.0f W/K, defrost takes "
                  "%.0f W)" % (len(dw), math.sqrt(ss / cnt), h["air_J_K"] / 1e6, h["air_mass_W_K"], -amp["defrost"]))
        wins = rolling_windows(test)
        score, _, _ = rolling(h, wins, chans, amp)
        print(score_lines("%d forecasts of unseen data" % len(wins), score, no_model(wins)) + "\n")


def cmd_residual(args):
    m = load_model()
    log = heatlog.find_log(args.log)
    segs = segments([log])
    chans, amp = model_channels(m, segs)
    print("Heat the room got beyond what the model accounts for, every two hours of %s." % log.name)
    print("A run of hours well above zero is a heat source nobody mentioned; well below, a loss (a window, wind) or")
    print("the heat estimate reading high. Told-of sources and the daily gain are already taken out.\n")
    for r in unexplained(m["house"], chans, amp, segs):
        bar = ("+" if r["W"] > 0 else "-") * min(30, int(abs(r["W"]) / 50))
        print("  %s  %+5.0f W  %-30s room %.2f  outdoor %+5.1f  unit %4.0f W%s" % (
            time.strftime("%a %m-%d %H:%M", time.localtime(r["t"])), r["W"], bar, r["room"], r["outdoor"], r["heat"],
            "  (few samples)" if r["n"] < 4 else ""))


def cmd_fit(args):
    old = load_model()
    usable = usable_series(args.hours)
    if not usable:
        raise SystemExit("no log has a heat output estimate: nothing to fit")
    segs = segments(usable)
    sources = load_sources()
    hours = sum(len(g["R"]) for g in segs) / 60.0
    print("Fitting on %.0f h in %d stretches from %d boots" % (hours, len(segs), len(usable)))
    for g in segs:
        print("  %s to %s  %s" % (stamp(g["t"][0]), stamp(g["t"][-1]), g["name"][:70]))
    print("Heat sources told of (events.json): %s" % (", ".join("%s x%d" % (k, len(v)) for k, v in sorted(
        sources.items())) or "none"))
    print("House (the spread between starts shows how well a figure is pinned down):")
    house, amp, chans, info = fit_house(segs, old["house"], sources)
    new = {"house": house, "defrost": fit_defrost(usable, old["defrost"]), "unit": fit_unit(usable, old["unit"])}
    if amp.get("defrost") is not None:
        new["defrost"]["room_W"] = amp["defrost"]
    amp["defrost"] = new["defrost"]["room_W"]
    new["gains"] = gains_from(amp, sources, info.get("reference_fan"))
    wins = rolling_windows(segs)
    score, _, _ = rolling(house, wins, chans, amp)
    base = no_model(wins)
    new["fit"] = {"date": time.strftime("%Y-%m-%d"), "hours": hours,
                  "rms_through_defrosts": info.get("rms_through_defrosts"),
                  "forecast_rms": {str(z // 60): score[z][0] for z in HORIZONS},
                  "no_model_rms": {str(z // 60): base[z] for z in HORIZONS},
                  "fitted_until": max(g["t"][-1] for g in segs), "logs": sorted({l.name for l in heatlog.all_logs()})}
    new["history"] = (old.get("history") or []) + ([{"date": old["fit"]["date"], "hours": old["fit"]["hours"],
                                                     "house": old["house"], "defrost": old["defrost"],
                                                     "unit": old["unit"], "gains": old.get("gains")}]
                                                   if old.get("fit") else [])
    print()
    show(old, "Before")
    show(new, "After")
    print("\nWhat moved (old -> new):")

    def moved(label, a, b, unit=""):
        ch = 100 * (b / a - 1) if a else 0
        print("  %-30s %10.4g -> %10.4g %s (%+.0f%%)%s" % (label, a, b, unit, ch, "   <-- over 20%" if abs(ch) > 20 else ""))
    for k in HOUSE:
        moved(k, old["house"][k], house[k])
    moved("overall loss W/K", ua(old["house"]), ua(house))
    for k in ("room_W", "seconds", "frost_Wh", "lead_s"):
        moved("defrost " + k, old["defrost"][k], new["defrost"][k])
    for k in ("heat_per_table", "q100q", "p100q", "q100", "p100"):
        moved("unit %s at 0 degC" % k, at(old["unit"][k], 0), at(new["unit"][k], 0))
        if abs(new["unit"][k].get("b", 0) - old["unit"][k].get("b", 0)) > 1e-9:
            print("  %-30s %10.4g -> %10.4g per degC" % ("  slope", old["unit"][k].get("b", 0), new["unit"][k]["b"]))
    moved("unit power_per_table", old["unit"]["power_per_table"], new["unit"]["power_per_table"])
    moved("unit ramp above 95", old["unit"]["ramp_high_W_min"], new["unit"]["ramp_high_W_min"], "W/min")
    print("\nRolling forecasts on the data just fitted (flattering; `holdout` and the next `validate` are the tests):")
    print(score_lines("model, %d forecasts: rms (mean) degC" % len(wins), score, base))
    if args.dry_run:
        print("\n--dry-run: model.json not written")
    else:
        heatlog.write_json(model_path(), new)
        print("\nsaved %s" % model_path())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["show", "validate", "fit", "holdout", "residual"])
    ap.add_argument("log", nargs="?")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--hours", type=float, default=240, help="fit, holdout: use only the latest this many hours")
    args = ap.parse_args()
    {"show": lambda a: show(load_model()), "validate": cmd_validate, "fit": cmd_fit, "holdout": cmd_holdout,
     "residual": cmd_residual}[args.command](args)


if __name__ == "__main__":
    main()
