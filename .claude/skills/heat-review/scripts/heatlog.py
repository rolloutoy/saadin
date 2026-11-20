"""Shared loader and measures for Saadin heat logs (saved /graph.json pulls).

A log is the device's heat log as columns, one sample per `interval` seconds, with `ago` (seconds before the pull)
as its only clock. Temperatures are in 1/100 degC, `scale` in 1/100. Everything here turns that into plain units and
absolute time, and works on older logs that lack the newer columns (they come back as None).
"""
import datetime
import json
import os
import re
import statistics as st
import time

DEFAULT_TABLE = "40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470"
CENTI = ("room", "outdoor", "target", "effective", "setpoint", "smoothed", "liquid", "intake", "scale")
ALIAS = {"deficit": ("owed",), "heattarget": ("give",)}  # Names used by firmware between PR 46's first and last push
DEMAND, DEFROST, HOLD = 2, 1, 2  # state "demand", sub "defrost", sub "hold"
SHORT_CYCLE = 45  # A defrost within this many minutes of the last is a short cycle (a defrost loop when they repeat)


def repo_root():
    p = os.path.dirname(os.path.abspath(__file__))
    while p != os.path.dirname(p):
        if os.path.isdir(os.path.join(p, "ESP", "main")):
            return p
        p = os.path.dirname(p)
    return os.getcwd()


def logdir():
    return os.environ.get("HEATLOG_DIR") or os.path.join(repo_root(), "graph-logs")


def read_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def parse_table(s):
    """demand.table -> list of (name, demand, quiet, watts) in order of watts"""
    out = []
    for part in (s or DEFAULT_TABLE).split(","):
        k, w = part.split(":")
        k = k.strip()
        q = k.endswith("q")
        d = int(k.rstrip("q"))
        out.append((k if d == 100 else str(d), d, 1 if (q or d < 100) else 0, float(w)))
    return sorted(out, key=lambda r: r[3])


def stamp(t, secs=False):
    return datetime.datetime.fromtimestamp(t).strftime("%m-%d %H:%M:%S" if secs else "%m-%d %H:%M")


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


class Series:
    """Columns in plain units on an absolute clock. Log loads one file; concat() joins pulls of one boot."""

    def __init__(self, name, t, interval, cols, table, states, subs):
        self.name, self.t, self.interval, self.cols = name, t, interval, cols
        self.table, self.states, self.subs = table, states, subs
        self.n = len(t)

    def has(self, key):
        return key in self.cols and any(v is not None for v in self.cols[key])

    def col(self, key):
        return self.cols.get(key) or [None] * self.n

    def level(self, i):
        """Demand level as the table names it ("95", "100q", "100"), None when not heating on demand"""
        d = self.col("demand")[i]
        if d is None or self.col("state")[i] != DEMAND:
            return None
        if d == 100:
            q = self.col("quiet")[i]
            return "100q" if (q is None or q) else "100"
        return str(d)

    def table_watts(self):
        return {name: w for name, _, _, w in self.table}

    def slice(self, a, b):
        return Series(self.name, self.t[a:b], self.interval, {k: v[a:b] for k, v in self.cols.items()}, self.table,
                      self.states, self.subs)

    def index_at(self, when):
        """First sample at or after an epoch time"""
        for i, x in enumerate(self.t):
            if x >= when:
                return i
        return self.n

    # --- defrosts and cycles
    def defrosts(self):
        """[(start, end)] sample ranges with the defrost sub-state, end exclusive"""
        sub, out, a = self.col("sub"), [], None
        for i in range(self.n):
            if sub[i] == DEFROST and a is None:
                a = i
            elif sub[i] != DEFROST and a is not None:
                out.append((a, i))
                a = None
        if a is not None:
            out.append((a, self.n))
        return out

    def cycles(self):
        """[(a, b)] from one defrost start to the next, whole cycles only, none across a gap in the log"""
        starts = [a for a, _ in self.defrosts() if a > 0]
        out = []
        for a, b in zip(starts, starts[1:]):
            if self.t[b] - self.t[a] <= (b - a) * self.interval * 1.2 + 60:
                out.append((a, b))
        return out

    def steady(self, settle=120, settle_top=360, ahead=240):
        """Mask of settled running: heating on demand, no defrost or hold, `settle` seconds into the level (the
        100% levels ramp for longer), and no defrost within `ahead` seconds (the unit winds down before one)"""
        sub, state = self.col("sub"), self.col("state")
        mask, since = [False] * self.n, 0
        k = int(ahead / self.interval) + 1
        for i in range(self.n):
            lv = self.level(i)
            if i == 0 or lv != self.level(i - 1) or sub[i] != sub[i - 1] or state[i] != state[i - 1]:
                since = i
            if lv is None or sub[i]:
                continue
            if (i - since) * self.interval < (settle_top if lv.startswith("100") else settle):
                continue
            if DEFROST in sub[i:i + k]:
                continue
            mask[i] = True
        return mask

    def mean(self, key, a, b):
        v = [x for x in self.col(key)[a:b] if x is not None]
        return sum(v) / len(v) if v else None


class Log(Series):
    def __init__(self, path, index=None):
        raw = read_json(path)
        if not isinstance(raw, dict) or "ago" not in raw:
            raise ValueError("%s is not a heat log" % path)
        name = os.path.basename(path)
        ago = raw["ago"]
        interval = raw.get("interval")
        if not interval:
            d = sorted(a - b for a, b in zip(ago, ago[1:]))
            interval = d[len(d) // 2] if d else 15
        pulled = ((index or {}).get(name) or {}).get("pulled") or pulled_from_name(name) or os.path.getmtime(path)
        cols = {}
        for k, v in raw.items():
            if isinstance(v, list) and len(v) == len(ago) and k not in ("states", "subs"):
                cols[k] = [None if x is None else x / 100.0 for x in v] if k in CENTI else v
        for new, olds in ALIAS.items():
            for old in olds:
                if new not in cols and old in cols:
                    cols[new] = cols[old]
        super().__init__(name, [pulled - a for a in ago], interval, cols, parse_table(raw.get("table")),
                         raw.get("states") or ["off", "idle", "demand", "cold", "nosensor"],
                         raw.get("subs") or ["", "defrost", "hold", "rest", "push"])
        self.path, self.pulled, self.raw = path, pulled, raw


def pulled_from_name(name):
    m = re.match(r"graph-(\d{4})-(\d\d)-(\d\d)T(\d\d)(\d\d)(\d\d)?", name)
    if not m:
        return None
    y, mo, d, h, mi, s = m.groups()
    # A name to the minute: the pull was somewhere in that minute, take its middle
    return time.mktime((int(y), int(mo), int(d), int(h), int(mi), int(s) if s else 30, 0, 0, -1))


def index_path():
    return os.path.join(logdir(), "index.json")


def all_logs():
    """Every saved log, oldest pull first"""
    d = logdir()
    index = read_json(index_path(), {})
    logs = []
    for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if re.match(r"graph-\d{4}-.*\.json$", f):
            try:
                logs.append(Log(os.path.join(d, f), index))
            except (ValueError, KeyError):
                pass
    return sorted(logs, key=lambda l: l.pulled)


def newest_log():
    logs = all_logs()
    if not logs:
        raise SystemExit("no logs in %s: run pull.py first" % logdir())
    return logs[-1]


def find_log(arg):
    """A path, a file name in the log directory, or nothing for the newest"""
    if not arg:
        return newest_log()
    path = arg if os.path.exists(arg) else os.path.join(logdir(), arg)
    return Log(path, read_json(index_path(), {}))


def concat(parts):
    cols = {}
    keys = set().union(*[p.cols.keys() for p in parts])
    for k in keys:
        cols[k] = []
        for p in parts:
            cols[k] += p.col(k)
    last = parts[-1]
    return Series("+".join(p.name for p in parts) if len(parts) > 1 else last.name, sum((p.t for p in parts), []),
                  last.interval, cols, last.table, last.states, last.subs)


def runs(logs=None):
    """The logs as one Series per boot, without the overlap between pulls of the same boot.

    The device log is a rolling buffer that starts again at a restart, so two pulls overlap in time only when no
    restart came between them. Newest first: an older pull adds only what came before the newer one starts.
    """
    logs = sorted(logs if logs is not None else all_logs(), key=lambda l: l.pulled, reverse=True)
    out = []  # [[parts in time order]]
    for log in logs:
        if not log.n:
            continue
        for parts in out:
            first = parts[0]
            if log.t[-1] >= first.t[0] - 2 * log.interval and log.interval == first.interval:  # Same boot
                cut = log.index_at(first.t[0] - log.interval / 2)
                if cut * log.interval > 120:  # Not for the odd sample that only the rounding of pull times adds
                    parts.insert(0, log.slice(0, cut))
                break
        else:
            out.append([log])
    return sorted((concat(p) for p in out), key=lambda s: s.t[0])


# --- measures shared by report.py, model.py and sim.py


def cycle_stats(s):
    """One dict per whole defrost cycle"""
    out = []
    H = s.interval / 3600.0
    room, eff, dem = s.col("room"), s.col("effective"), s.col("demand")
    for a, b in s.cycles():
        err = [room[i] - eff[i] for i in range(a, b) if room[i] is not None and eff[i] is not None]
        r = [x for x in room[a:b] if x is not None]
        if not err or s.mean("power", a, b) is None:
            continue
        heat = s.mean("heatout", a, b)
        power = s.mean("power", a, b)
        lv = [s.level(i) for i in range(a, b)]
        d = {
            "a": a, "b": b, "t": s.t[a], "minutes": (b - a) * s.interval / 60.0,
            "outdoor": s.mean("outdoor", a, b), "err": sum(err) / len(err),
            "rms": (sum(e * e for e in err) / len(err)) ** 0.5, "lo": min(r), "hi": max(r),
            "heat": heat, "power": power, "cop": heat / power if heat is not None and power else None,
            "at100q": sum(1 for x in lv if x == "100q") / (b - a), "at100": sum(1 for x in lv if x == "100") / (b - a),
            "demand_time": sum(1 for x in lv if x is not None) / (b - a),
            "changes": sum(1 for i in range(a + 1, b) if dem[i] != dem[i - 1] or lv[i - a] != lv[i - a - 1]),
            "big": sum(1 for i in range(a + 1, b) if lv[i - a] != lv[i - a - 1] and
                       ((lv[i - a] or "").startswith("100") or (lv[i - a - 1] or "").startswith("100"))),
            "frost_wh": sum(max(x or 0, 0) for x in s.col("heatout")[a:b]) * H if heat is not None else None,
            "request": s.mean("heatw", a, b) if s.has("deficit") else None,
            "integral": s.mean("integral", a, b) if s.has("deficit") else None,
        }
        out.append(d)
    return out


def level_stats(series_list):
    """Settled power and heat per (level, fan setting, 5 degC outdoor band) over several series"""
    groups = {}
    for s in series_list:
        mask = s.steady()
        for i in range(s.n):
            if not mask[i] or s.col("power")[i] is None or s.col("outdoor")[i] is None:
                continue
            key = (s.level(i), s.col("fanset")[i], int(s.col("outdoor")[i] // 5) * 5)
            groups.setdefault(key, []).append((s.col("power")[i], s.col("heatout")[i], s.col("comp")[i],
                                               s.col("outdoor")[i], s.interval))
    out = []
    for (lv, fan, band), v in groups.items():
        heat = [x[1] for x in v if x[1] is not None]
        pw = [x[0] for x in v]
        out.append({"level": lv, "fan": fan, "band": band, "minutes": sum(x[4] for x in v) / 60.0,
                    "power": st.median(pw), "p10": pct(pw, 0.1), "p90": pct(pw, 0.9),
                    "heat": st.median(heat) if heat else None,
                    "cop": (sum(heat) / len(heat)) / (sum(pw) / len(pw)) if heat else None,
                    "comp": st.median([x[2] for x in v if x[2] is not None] or [0]),
                    "outdoor": sum(x[3] for x in v) / len(v)})
    return out
