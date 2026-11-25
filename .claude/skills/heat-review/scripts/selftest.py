#!/usr/bin/env python3
"""Tests of the skill's own scripts, on logs made up here: nothing from a device or from the network is used.

Usage: python3 -I selftest.py [name ...]      (all tests, or those whose name contains one of the words)

Run it after changing a script. The state directory is a temporary one, so graph-logs/ is not touched. Needs gcc
for the tests that build the plant.
"""
import calendar
import contextlib
import io
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402
import model  # noqa: E402
import pull  # noqa: E402
import sim  # noqa: E402
import weather  # noqa: E402

T0 = 1794787200.0  # Start of every made-up log: a whole hour
HOUSE = {"air_J_K": 5.0e6, "air_mass_W_K": 1500.0, "mass_J_K": 6.0e7, "mass_out_W_K": 100.0}
DEFROST_W, SUN_M2 = -3000.0, 6.0
CLOUD = (1.0, 0.15, 0.7, 0.05, 0.9, 0.4, 0.2, 0.8)  # Share of a clear day's sun, day by day


def sun_at(t):
    """Made-up global radiation (W/m2): a half sine from 8 to 16 by the log's own clock, dimmer on cloudy days"""
    h = (t - T0) / 3600.0
    day, hour = int(h // 24), h % 24
    return 350.0 * CLOUD[day % len(CLOUD)] * math.sin(math.pi * (hour - 8) / 8) if 8 < hour < 16 else 0.0


def make_raw(hours, interval=30, sun=False):
    """A log as /graph.json gives it, from a two-node house with HOUSE's figures and a defrost every 70 min"""
    n = int(hours * 3600 / interval)
    ca, kam, cm, kmo = (HOUSE[k] for k in model.HOUSE)
    target, room = 21.0, 21.0
    mass = room - model.ua(HOUSE) * (room + 2.0) / kam
    keys = ("room", "outdoor", "power", "heatout", "effective", "target", "demand", "quiet", "state", "sub", "fanset")
    raw = {k: [] for k in keys}
    for i in range(n):
        t = i * interval
        outdoor = -2.0 + 2.0 * math.sin(2 * math.pi * t / 86400.0)
        phase = (t / 60.0) % 70
        defrost = t >= 70 * 60 and phase < 8
        q = 0.0 if defrost else max(500.0, min(5000.0, 2300.0 + 1500.0 * (target - room)))
        gain = SUN_M2 * sun_at(T0 + t) if sun else 0.0
        for _ in range(interval // 5):
            droom = (kam * (mass - room) + (DEFROST_W if defrost else q) + gain) / ca
            mass += (kam * (room - mass) + kmo * (outdoor - mass)) / cm * 5
            room += droom * 5
        for k, v in (("room", round(room * 100)), ("outdoor", round(outdoor * 2) * 50), ("power", round(q / 3.5)),
                     ("heatout", round(q)), ("effective", 2100), ("target", 2100), ("demand", 95), ("quiet", 1),
                     ("state", heatlog.DEMAND), ("sub", heatlog.DEFROST if defrost else heatlog.HOLD
                                                 if t >= 70 * 60 and phase < 18 else 0), ("fanset", 3)):
            raw[k].append(v)
    raw.update({"interval": interval, "table": heatlog.DEFAULT_TABLE, "ago": [(n - 1 - i) * interval for i in range(n)]})
    return raw


def save_log(d, raw, start=T0, name=None):
    """Write a made-up log so that its first sample is at `start`"""
    pulled = start + raw["ago"][0]
    name = name or time.strftime("graph-%Y-%m-%dT%H%M.json", time.localtime(pulled))
    with open(os.path.join(d, name), "w") as f:
        json.dump(raw, f)
    index = heatlog.read_json(os.path.join(d, "index.json"), {}) or {}
    index[name] = {"pulled": pulled}
    heatlog.write_json(os.path.join(d, "index.json"), index)
    return name


def save_weather(d, hours):
    """Made-up hourly weather for the first `hours` hours from T0, as weather.py keeps it"""
    rows = {}
    for h in range(1, int(hours) + 2):
        mid = T0 + h * 3600 - 1800
        rows[str(int(T0 + h * 3600))] = {"sun": sun_at(mid), "wind": 3.0 + 2.0 * math.sin(h / 7.0), "rh": 85.0, "ta": -2.0}
    heatlog.write_json(os.path.join(d, "weather.json"), {"hours": rows})


def script(name, *args, env=None):
    """Run one of the skill's scripts as the skill does; returns (exit code, all it printed)"""
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, name)] + list(args), capture_output=True, text=True,
                       env=e)
    return r.returncode, r.stdout + r.stderr


def quiet(fn, *args, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kw)


# --- the tests; each gets an empty state directory


def test_gap_between_minutes_splits_the_data(d):
    raw = make_raw(12)
    save_log(d, raw)
    s = heatlog.newest_log()
    for cutat in (480, 481):  # Two samples to the minute: 480 is between two minutes, 481 inside one
        joined = heatlog.concat([s.slice(0, cutat), s.slice(cutat + 120, s.n)])  # An hour missing
        segs = model.segments([joined])
        assert len(segs) == 2, "an hour missing after sample %d: %d stretch(es), want 2" % (cutat, len(segs))


def test_plant_ignores_the_callers_environment(d):
    m = model.load_model()
    scen = sim.parse_scen("-11")
    clean = sim.run(m, scen, {})
    old = dict(os.environ)
    os.environ.update({"FULL": "0", "SENSLAG": "600", "TARGET": "15", "KP": "5000"})
    try:
        dirty = sim.run(m, scen, {})
    finally:
        os.environ.clear()
        os.environ.update(old)
    assert clean == dirty, "variables in the caller's environment changed the plant's result"


def test_robust_rows_score_the_burst(d):
    rc, out = script("sim.py", "sweep", "--scen", "-1", "--var", "burst:PULSE_W=1500", "--robust")
    assert rc == 0, out
    rows = [l for l in out.splitlines() if "harsher plants" in l]
    assert len(rows) == 2 and rows[0] != rows[1], "the harsher plants gave the burst the same figures as no burst"


def test_fit_without_logs_says_so(d):
    rc, out = script("model.py", "fit")
    assert rc and "Traceback" not in out and "no log" in out, out


def test_too_little_data_is_not_called_a_fit(d):
    save_log(d, make_raw(10))  # Seven defrosts with room around them, and under a day
    rc, out = script("model.py", "fit")
    assert rc == 0, out
    m = heatlog.read_json(os.path.join(d, "model.json"))
    assert m["fit"]["house_fitted"] == [], m["fit"]["house_fitted"]
    assert m["house"] == model.START["house"]
    assert "STARTING VALUES" in model.fit_label(m)
    rc, out = script("model.py", "show")
    assert "starting value" in out, out
    rc, out = script("model.py", "holdout")
    assert rc == 0 and "0.00 (+0.00)" not in out and "too little" in out, out


def test_fit_uses_every_log_by_default(d):
    save_log(d, make_raw(8), start=T0)
    save_log(d, make_raw(8), start=T0 + 20 * 86400)
    rc, out = script("model.py", "fit", "--dry-run")
    assert rc == 0 and "from 2 boots" in out, out
    rc, out = script("model.py", "fit", "--dry-run", "--hours", "48")
    assert rc == 0 and "from 1 boots" in out, out


def test_fit_recovers_a_known_house(d):
    save_log(d, make_raw(96))
    segs = model.segments(heatlog.runs())
    h, amp, _, info = quiet(model.fit_house, segs, model.START["house"], {}, None)
    assert info["fitted"] == ["fast", "slow"], info
    # Air to mass is left out: it trades off against the other two fast figures along a flat valley (model.md)
    for k in ("air_J_K", "mass_J_K", "mass_out_W_K"):
        assert abs(h[k] / HOUSE[k] - 1) < 0.15, "%s fitted %.4g, the log was made with %.4g" % (k, h[k], HOUSE[k])
    assert abs(amp["defrost"] / DEFROST_W - 1) < 0.15, "defrost fitted %.0f W, made with %.0f" % (amp["defrost"], DEFROST_W)


def test_fit_finds_the_sun(d):
    save_log(d, make_raw(144, sun=True))
    save_weather(d, 144)
    segs = model.segments(heatlog.runs())
    w = weather.load()
    assert w and w.coverage(segs, "sun") > 0.99
    h, amp, _, info = quiet(model.fit_house, segs, model.START["house"], {}, w)
    assert abs(amp["sun"] / SUN_M2 - 1) < 0.35, "sun fitted %.2f W per W/m2, the log was made with %.2f" % (
        amp["sun"], SUN_M2)
    assert "wind" in amp
    h, amp, _, info = quiet(model.fit_house, segs, model.START["house"], {}, None)
    assert "sun" not in amp and "wind" not in amp


SETTINGS_PAGE = """<tr><td><tt><b>pid</b>kp<i></i></tt></td><td nowrap><input id="pidkp" name="_pidkp"
 onchange="this.name='pidkp';" value="1200" autocapitalize='off' placeholder="" style="text-align:right" size=10></td></tr>
<input id="idleover" name="_idleover" onchange="this.name='idleover';" value="0.4" size=10>
<input id="autocurve" name="_autocurve" onchange="this.name='autocurve';" value="-20:1.0,0:0" size=40>
<input id="idlefanmap" name="_idlefanmap" onchange="this.name='idlefanmap';" value="" size=40>
<input type=checkbox id="demandfull" name="_demandfull" onchange="this.name='demandfull';settings.__demandfull.name='demandfull';" checked>
<input type=checkbox id="defrostwatchdog" name="_defrostwatchdog" onchange="this.name='defrostwatchdog';">
<select name="_demandfan" onchange="this.name='demandfan';"><option value="0">Leave</option><option value="8" selected>Outdoor</option></select>
<input id="wifissid" name="_wifissid" value="not a heating setting">"""


def test_settings_are_read_from_the_devices_page(d):
    got = pull.heating_settings(SETTINGS_PAGE)
    assert got == {"pid.kp": 1200, "idle.over": 0.4, "auto.curve": "-20:1.0,0:0", "idle.fanmap": "", "demand.full": 1,
                   "defrost.watchdog": 0, "demand.fan": "Outdoor"}, got
    assert pull.heating_settings("<input name=password size=40 type=password autofocus>") == {}


def test_plant_runs_on_the_settings_on_record(d):
    path = os.path.join(d, "device.json")
    heatlog.write_json(path, {"settings": {"auto.t": 21.5, "auto.curve": "-20:1.0,0:0", "pid.kp": 1200,
                                           "defrost.watchdog": 0, "idle.min": 45}, "target": 22.0})
    env = sim.device_env()
    assert (env["TARGET"], env["CURVE"], env["KP"], env["WATCHDOG"], env["IDLE_MIN"]) == (
        21.5, "-20:1.0,0:0", 1200, 0, 45), env
    # With no auto.t on record there is only the effective target of the last pull, and the curve is in that already
    heatlog.write_json(path, {"settings": {"auto.curve": "-20:1.0,0:0"}, "target": 22.0})
    env = sim.device_env()
    assert env["TARGET"] == 22.0 and "CURVE" not in env, env


def test_plant_follows_the_target_curve(d):
    m = model.load_model()
    scen = sim.parse_scen("-10")
    demand = {"TARGET": 21, "COLD_ENTER": -30, "COLD_EXIT": -25}  # Demand control: the plant does not model the setpoint
    flat = sim.run(m, scen, dict(demand, CURVE=""))["steady"]
    bent = sim.run(m, scen, dict(demand, CURVE="-20:1.0,0:0"))["steady"]  # Half a degree up at -10
    shift = (bent["lo"] + bent["hi"]) / 2 - (flat["lo"] + flat["hi"]) / 2
    assert 0.35 < shift < 0.65, "the room moved %+.2f degC with a curve that asks for +0.50" % shift
    assert abs(bent["mean"]) < 0.1, "mean error %+.2f against the target with the curve in it" % bent["mean"]


FMI_REPLY = """<wfs:member><BsWfs:BsWfsElement gml:id="BsWfsElement.1.1.1"><BsWfs:Location><gml:Point><gml:pos>60.0 25.0 </gml:pos>
</gml:Point></BsWfs:Location><BsWfs:Time>2026-11-15T02:00:00Z</BsWfs:Time>
<BsWfs:ParameterName>WS_PT1H_AVG</BsWfs:ParameterName><BsWfs:ParameterValue>5.6</BsWfs:ParameterValue></BsWfs:BsWfsElement>
</wfs:member><wfs:member><BsWfs:BsWfsElement><BsWfs:Time>2026-11-15T02:00:00Z</BsWfs:Time>
<BsWfs:ParameterName>RH_PT1H_AVG</BsWfs:ParameterName><BsWfs:ParameterValue>NaN</BsWfs:ParameterValue></BsWfs:BsWfsElement>
</wfs:member>"""


def test_weather_reply_is_read(d):
    rows = weather.parse_reply(FMI_REPLY)
    at = calendar.timegm((2026, 11, 15, 2, 0, 0))
    assert rows == [(at, "WS_PT1H_AVG", 5.6), (at, "RH_PT1H_AVG", None)], rows
    # Radiation comes as samples within the hour: the hour that ends at `at` is their mean, never below zero
    hours = weather.hourly_mean([(at - 3000, "GLOB_1MIN", 100.0), (at - 600, "GLOB_1MIN", 200.0), (at, "GLOB_1MIN", -3.0),
                                 (at + 600, "GLOB_1MIN", 50.0)])
    assert hours == {at: 100.0, at + 3600: 50.0}, hours
    heatlog.write_json(os.path.join(d, "weather.json"), {"hours": {str(at): {"wind": 5.6, "rh": None}}})
    w = weather.load()
    assert w.at(at - 1, "wind") == 5.6 and w.at(at - 3599, "wind") == 5.6  # The hour before the stamp
    assert w.at(at - 1, "rh") is None and w.at(at + 4 * 3600, "wind") is None


def test_nearest_station(d):
    stations = [{"fmisid": 1, "name": "near", "lat": 60.1, "lon": 25.0}, {"fmisid": 2, "name": "far", "lat": 62.0, "lon": 25.0}]
    s = weather.nearest(stations, 60.0, 25.0)
    assert s["fmisid"] == 1 and 10 < s["km"] < 12, s


def main():
    want = sys.argv[1:]
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and
             (not want or any(w in k for w in want))]
    failed = 0
    for name, fn in tests:
        d = tempfile.mkdtemp(prefix="heat-review-test-")
        os.environ["HEATLOG_DIR"] = d
        try:
            fn(d)
            print("ok    %s" % name)
        except Exception as e:  # A failed assert, or the script under test falling over
            failed += 1
            print("FAIL  %s: %s: %s" % (name, type(e).__name__, str(e).strip()[-1500:]))
        finally:
            shutil.rmtree(d, ignore_errors=True)
    print("\n%d tests, %d failed" % (len(tests), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
