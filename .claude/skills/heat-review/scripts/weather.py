#!/usr/bin/env python3
"""Sun, wind and humidity by the hour from the Finnish Meteorological Institute's open data, kept in graph-logs/.

Usage: python3 -I weather.py locate --place NAME | --latlon LAT,LON
       python3 -I weather.py [pull] [--days N]
       python3 -I weather.py show

  locate  find the weather station and the radiation station nearest to the house and note them in
          graph-logs/location.json. NAME is a town or village as FMI knows it; coordinates are kept to two decimals
  pull    fetch every hour from the start of the oldest saved log (or the last N days) that is not yet in
          graph-logs/weather.json, and add it there
  show    what is on record: the stations, the hours held, and the last day of them

Only GET requests to opendata.fmi.fi are made, with no key. What is sent is the station numbers and a time span
(and, for `locate --place`, the place name). The outdoor temperature the model runs on stays the unit's own sensor:
the station's temperature is kept only because its humidity is relative to it.

Hours are stamped as FMI stamps them, by their end: the value at 14:00 UTC is the mean of 13:00 to 14:00.
"""
import argparse
import calendar
import math
import os
import re
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402

API = "https://opendata.fmi.fi/wfs"
HOURLY = {"WS_PT1H_AVG": "wind", "RH_PT1H_AVG": "rh", "TA_PT1H_AVG": "ta"}  # m/s, %, degC at the weather station
SUN = "GLOB_1MIN"  # Global radiation, W/m2: one-minute means, fetched every 10 min and averaged over the hour
KEYS = ("sun", "wind", "rh", "ta")
NETWORKS = {"weather": "Automaattinen sääasema", "radiation": "Auringonsäteilyasema"}
CHUNK = 6 * 86400  # Longest span asked for in one request
NEAR = 3 * 3600    # An hour with no value takes the nearest one within this


def location_path():
    return os.path.join(heatlog.logdir(), "location.json")


def weather_path():
    return os.path.join(heatlog.logdir(), "weather.json")


def get(query, **params):
    params.update({"service": "WFS", "version": "2.0.0", "request": "getFeature", "storedquery_id": query})
    with urllib.request.urlopen(API + "?" + urllib.parse.urlencode(params), timeout=90) as r:
        return r.read().decode("utf-8", "replace")


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def parse_reply(text):
    """[(epoch, parameter, value or None)] from a reply in FMI's "simple" form"""
    out = []
    for when, name, value in re.findall(
            r"<BsWfs:Time>([^<]+)</BsWfs:Time>\s*<BsWfs:ParameterName>([^<]+)</BsWfs:ParameterName>\s*"
            r"<BsWfs:ParameterValue>([^<]*)</BsWfs:ParameterValue>", text):
        try:
            v = float(value)
        except ValueError:
            v = None
        out.append((calendar.timegm(time.strptime(when.strip(), "%Y-%m-%dT%H:%M:%SZ")), name.strip(),
                    None if v is None or math.isnan(v) else v))
    return out


def hourly_mean(rows):
    """{hour end: mean} of samples within each hour, a sample on the hour belonging to the hour it ends. Never
    below zero: a radiation sensor reads a little under it at night"""
    acc = {}
    for t, _, v in rows:
        if v is not None:
            acc.setdefault(int(math.ceil(t / 3600.0)) * 3600, []).append(max(v, 0.0))
    return {h: sum(v) / len(v) for h, v in acc.items()}


def parse_stations(text):
    """Stations still in use, from FMI's list of them: [{fmisid, name, lat, lon, networks}]"""
    out = []
    for f in re.findall(r"<ef:EnvironmentalMonitoringFacility .*?</ef:EnvironmentalMonitoringFacility>", text, re.S):
        sid = re.search(r'stationcode/fmisid">(\d+)<', f)
        name = re.search(r"<ef:name>([^<]*)<", f)
        pos = re.search(r"<gml:pos>\s*([-\d.]+)\s+([-\d.]+)", f)
        if sid and name and pos and 'indeterminatePosition="now"' in f:
            out.append({"fmisid": int(sid.group(1)), "name": name.group(1), "lat": float(pos.group(1)),
                        "lon": float(pos.group(2)), "networks": re.findall(r'belongsTo xlink:title="([^"]*)"', f)})
    return out


def km(lat1, lon1, lat2, lon2):
    a, b = math.radians(lat1), math.radians(lat2)
    h = math.sin((b - a) / 2) ** 2 + math.cos(a) * math.cos(b) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 6371.0 * 2 * math.asin(math.sqrt(h))


def nearest(stations, lat, lon):
    s = min(stations, key=lambda s: km(lat, lon, s["lat"], s["lon"]))
    return {"fmisid": s["fmisid"], "name": s["name"], "km": round(km(lat, lon, s["lat"], s["lon"]), 1)}


class Weather:
    """The hours on record. at() gives a value for any time, coverage() says how much of some data has one"""

    def __init__(self, hours, stations=None):
        self.hours, self.stations = hours, stations or {}

    def at(self, t, key):
        h = (int(t // 3600) + 1) * 3600
        for step in range(0, NEAR + 1, 3600):
            for x in ((h,) if not step else (h - step, h + step)):
                v = (self.hours.get(x) or {}).get(key)
                if v is not None:
                    return v
        return None

    def coverage(self, segs, key):
        """Share of the minutes in these stretches of data that have a value"""
        t = [x for g in segs for x in g["t"]]
        return sum(1 for x in t if self.at(x, key) is not None) / float(len(t)) if t else 0.0

    def mean(self, key, a, b):
        """Mean over the hours that a..b (epoch seconds) touches, None if none of them has a value"""
        v = [self.at(h, key) for h in range(int(a // 3600) * 3600, int(b // 3600) * 3600 + 1, 3600)]
        v = [x for x in v if x is not None]
        return sum(v) / len(v) if v else None


def load():
    """The weather on record, or None when there is none"""
    raw = heatlog.read_json(weather_path())
    if not raw or not raw.get("hours"):
        return None
    return Weather({int(k): v for k, v in raw["hours"].items()}, raw.get("stations"))


def cmd_locate(args):
    if args.latlon:
        try:
            lat, lon = (round(float(x), 2) for x in args.latlon.split(","))
        except ValueError:
            raise SystemExit("--latlon takes LAT,LON in degrees, for example 61.5,23.8")
        place = None
    elif args.place:
        # FMI turns a place name into its nearest weather station; that station's position stands for the place
        reply = get("fmi::observations::weather::hourly::simple", place=args.place, maxlocations=1,
                    starttime=iso(time.time() - 6 * 3600), parameters="TA_PT1H_AVG")
        pos = re.search(r"<gml:pos>\s*([-\d.]+)\s+([-\d.]+)", reply)
        if not pos:
            raise SystemExit("FMI knows no weather station for the place \"%s\": try the town's name, or --latlon" %
                             args.place)
        lat, lon, place = round(float(pos.group(1)), 2), round(float(pos.group(2)), 2), args.place
    else:
        raise SystemExit("give --place NAME or --latlon LAT,LON")
    stations = parse_stations(get("fmi::ef::stations"))
    loc = {"lat": lat, "lon": lon, "place": place, "set": time.strftime("%Y-%m-%d")}
    for kind, network in NETWORKS.items():
        sel = [s for s in stations if network in s["networks"]]
        if not sel:
            raise SystemExit("FMI's station list has no station of the kind \"%s\"" % network)
        loc[kind] = nearest(sel, lat, lon)
    os.makedirs(heatlog.logdir(), exist_ok=True)
    heatlog.write_json(location_path(), loc)
    print("saved %s" % location_path())
    show_location(loc)


def show_location(loc):
    print("  house at about %.2f, %.2f%s" % (loc["lat"], loc["lon"], " (%s)" % loc["place"] if loc.get("place") else ""))
    print("  wind and humidity from %s (station %d), %.0f km away" % (
        loc["weather"]["name"], loc["weather"]["fmisid"], loc["weather"]["km"]))
    print("  sun from %s (station %d), %.0f km away%s" % (
        loc["radiation"]["name"], loc["radiation"]["fmisid"], loc["radiation"]["km"],
        ": far enough for its clouds to be other clouds; trust days, not hours" if loc["radiation"]["km"] > 50 else ""))


def cmd_pull(args):
    loc = heatlog.read_json(location_path())
    if not loc:
        raise SystemExit("no location on record: ask the user where the house is (a town is enough), then run "
                         "weather.py locate --place NAME or --latlon LAT,LON")
    raw = heatlog.read_json(weather_path(), {}) or {}
    hours = {int(k): v for k, v in (raw.get("hours") or {}).items()}
    now = int(time.time() // 3600) * 3600
    if args.days:
        start = now - int(args.days * 86400)
    else:
        logs = heatlog.all_logs()
        if not logs:
            raise SystemExit("no logs in %s to fetch weather for: run pull.py first, or give --days N" % heatlog.logdir())
        start = int(min(l.t[0] for l in logs if l.n) // 3600) * 3600
    # Hours never asked for, and recent ones that still lack a value: FMI is an hour or two behind, and a station
    # that was silent for an hour two days ago will not fill it in later
    asked = raw.get("asked_until") or 0
    need = [h for h in range(start + 3600, now + 1, 3600) if h not in hours or (
        h > asked and any(hours[h].get(k) is None for k in KEYS))]
    if not need:
        print("weather is complete from %s to %s: nothing to fetch" % (heatlog.stamp(start), heatlog.stamp(now)))
        return
    got = {k: 0 for k in KEYS}
    for a in range(need[0] - 3600, need[-1], CHUNK):
        b = min(a + CHUNK, need[-1])
        try:
            rows = parse_reply(get("fmi::observations::weather::hourly::simple", fmisid=loc["weather"]["fmisid"],
                                   starttime=iso(a), endtime=iso(b), parameters=",".join(HOURLY)))
            sun = hourly_mean(parse_reply(get("fmi::observations::radiation::simple", timestep=10, parameters=SUN,
                                              fmisid=loc["radiation"]["fmisid"], starttime=iso(a + 600), endtime=iso(b))))
        except Exception as e:  # Unreachable, or an error reply
            print("WEATHER PULL FAILED: %s" % e)
            return 2
        for t, name, v in rows:
            if name in HOURLY and v is not None and t % 3600 == 0:
                hours.setdefault(t, {})[HOURLY[name]] = v
                got[HOURLY[name]] += 1
        for t, v in sun.items():
            hours.setdefault(t, {})["sun"] = round(v, 1)
            got["sun"] += 1
    for h in need:
        hours.setdefault(h, {})
    heatlog.write_json(weather_path(), {"hours": {str(k): hours[k] for k in sorted(hours)}, "pulled": time.time(),
                                        "asked_until": max(asked, min(need[-1], now - 2 * 86400)),
                                        "stations": {k: loc[k] for k in NETWORKS}})
    print("saved %s" % weather_path())
    print("  %s to %s: %s" % (heatlog.stamp(need[0] - 3600), heatlog.stamp(need[-1]), ", ".join(
        "%s %d h" % (k, got[k]) for k in KEYS)))
    left = [h for h in need if any((hours.get(h) or {}).get(k) is None for k in ("sun", "wind", "rh"))]
    if len(left) > 3:  # The newest hour or two are not out yet at any time
        print("  %d hours still lack a value (%s to %s): the station did not report, or not yet" % (
            len(left), heatlog.stamp(left[0]), heatlog.stamp(left[-1])))


def cmd_show(args):
    loc = heatlog.read_json(location_path())
    if not loc:
        print("no location on record (weather.py locate)")
        return
    show_location(loc)
    w = load()
    if not w:
        print("  no weather on record yet (weather.py pull)")
        return
    hs = sorted(w.hours)
    print("  %d hours on record, %s to %s" % (len(hs), heatlog.stamp(hs[0]), heatlog.stamp(hs[-1])))
    print("  hour ending    sun W/m2  wind m/s  humidity %  station degC")
    for h in hs[-24:]:
        r = w.hours[h]
        print("  %s  %s" % (heatlog.stamp(h), "  ".join(
            ("%8.1f" % r[k]) if r.get(k) is not None else "       -" for k in KEYS)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="pull", choices=["locate", "pull", "show"])
    ap.add_argument("--place", help="locate: a town or village name")
    ap.add_argument("--latlon", help="locate: LAT,LON in degrees")
    ap.add_argument("--days", type=float, help="pull: the last this many days, whatever logs are saved")
    args = ap.parse_args()
    try:
        return {"locate": cmd_locate, "pull": cmd_pull, "show": cmd_show}[args.command](args)
    except OSError as e:  # Unreachable, or FMI refused the request
        print("FMI REQUEST FAILED: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
