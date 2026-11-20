#!/usr/bin/env python3
"""Pull the heat log from the device and save it in graph-logs/.

Usage: python3 -I pull.py [--host saadin.local]

Only GET requests are made: /graph.json (the log), /graph (firmware id in the footer) and /revk-settings (the page
that lists the firmware's own settings, as the web interface opens it; the heating settings in a fixed list are read
from it and nothing else on it is kept).
Writes graph-logs/graph-<local time>.json unchanged from the device, and notes the pull in graph-logs/index.json
and the heating settings the device is running on in graph-logs/device.json.
"""
import argparse
import gzip
import html
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # Keep the skill folder to its sources
import heatlog  # noqa: E402
import report  # noqa: E402

# The settings the heating controller runs on (heatctl_settings() in Faikout.c) and those behind the heat estimate,
# as the settings page names its fields (the setting's name without the dot). A new heating setting goes here
PAGE_SETTINGS = {f.replace(".", ""): f for f in (
    "auto.e", "auto.t", "auto.curve", "auto.stale", "auto.smooth",
    "demand.set", "demand.table", "demand.cop", "demand.full", "demand.start", "demand.rate", "demand.minrun",
    "demand.fan", "demand.fanmap", "pid.kp", "pid.ti",
    "idle.temp", "idle.over", "idle.under", "idle.min", "idle.fan", "idle.fanmap",
    "cold.enter", "cold.exit", "cold.hold", "cold.set", "cold.rate", "cold.kp", "cold.ti", "cold.fan", "cold.fanmap",
    "defrost.enter", "defrost.exit", "defrost.max", "defrost.hold", "defrost.holdmax", "defrost.push", "defrost.pushto", "defrost.pushmax", "defrost.pushalt",
    "defrost.watchdog",
    "defrost.loops", "defrost.window", "defrost.rest", "fan.flow", "fan.hold")}
SETTINGS_PAGES = (-2, 0)  # All the firmware's own settings; the heating page, for firmware that lacks the first


def get(url, timeout):
    # The device gzips the log when asked, which makes a pull several times shorter
    req = urllib.request.Request(url, headers={"Accept-Encoding": "gzip"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
        return gzip.decompress(body) if r.headers.get("Content-Encoding") == "gzip" else body


def num(text):
    for conv in (int, float):
        try:
            return conv(text)
        except ValueError:
            pass
    return text


def heating_settings(page):
    """{setting: value} for the heating settings on a settings page of the device, in the units the page shows:
    a number where it is one, else the text, 1 or 0 for a switch, the chosen name for a choice"""
    out = {}
    for tag in re.findall(r"<input\b[^>]*>", page):
        field = re.search(r'\sid="([^"]+)"', tag)
        if not field or field.group(1) not in PAGE_SETTINGS:
            continue
        value = re.search(r'\svalue="([^"]*)"', tag)
        if "type=checkbox" in tag:
            out[PAGE_SETTINGS[field.group(1)]] = 1 if re.search(r"\schecked\b", tag) else 0
        elif value:
            out[PAGE_SETTINGS[field.group(1)]] = num(html.unescape(value.group(1)))
    for field, options in re.findall(r'<select name="_([^"]+)"[^>]*>(.*?)</select>', page, re.S):
        chosen = re.search(r"<option[^>]*\sselected[^>]*>([^<]*)<", options)
        if field in PAGE_SETTINGS and chosen:
            out[PAGE_SETTINGS[field]] = html.unescape(chosen.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=None)
    args = ap.parse_args()
    d = heatlog.logdir()
    os.makedirs(d, exist_ok=True)
    device_path = os.path.join(d, "device.json")
    device = heatlog.read_json(device_path, {}) or {}
    host = args.host or device.get("host") or "saadin.local"
    try:
        body = get("http://%s/graph.json" % host, 90)
        pulled = time.time()
        raw = json.loads(body)
    except Exception as e:  # Unreachable, or not a log
        print("PULL FAILED: http://%s/graph.json: %s" % (host, e))
        return 2
    n = len(raw.get("ago") or [])
    if not n or any(isinstance(v, list) and len(v) != n for k, v in raw.items() if k not in ("states", "subs")):
        print("PULL FAILED: the reply is not a heat log with equal columns (%d samples)" % n)
        return 2
    name = time.strftime("graph-%Y-%m-%dT%H%M.json", time.localtime(pulled))
    if os.path.exists(os.path.join(d, name)):
        name = time.strftime("graph-%Y-%m-%dT%H%M%S.json", time.localtime(pulled))
    path = os.path.join(d, name)
    with open(path, "wb") as f:
        f.write(body)

    firmware = build = None
    try:
        page = get("http://%s/graph" % host, 20).decode("utf-8", "replace")
        m = re.search(r":\s*([0-9a-f]{7,40})\s+(\d{4}-\d\d-\d\dT[\d:]+)\s*</address>", page)
        if m:
            firmware, build = m.groups()
    except Exception:
        pass

    # The settings the device is running on, from its own settings page
    seen, locked = {}, False
    for number in SETTINGS_PAGES:
        try:
            page = get("http://%s/revk-settings?_page=%d" % (host, number), 20).decode("utf-8", "replace")
        except Exception:
            continue
        locked = locked or bool(re.search(r"<input name=password[^>]*type=password", page))
        for k, v in heating_settings(page).items():
            seen.setdefault(k, v)
    if raw.get("table"):
        seen["demand.table"] = raw["table"]  # As the firmware parsed it: the page is empty for the default
    read = len(seen)
    # Where the page did not give them (a password on it, older firmware): what the log itself gives away
    for col, setting in (("kp", "pid.kp"), ("ti", "pid.ti")):  # As in use at the last sample (in demand control)
        v = [x for x in raw.get(col) or [] if x is not None]
        if v and raw.get("state") and raw["state"][-1] == heatlog.DEMAND:
            seen.setdefault(setting, v[-1])
    log = heatlog.Log(path, {name: {"pulled": pulled}})
    shown = report.inferred(log)
    if "defrost.hold" in shown:
        seen.setdefault("defrost.hold", int(round(shown["defrost.hold"])))  # Minutes; a hold reads a sample short
    if "demand.rate" in shown:
        seen.setdefault("demand.rate", int(round(shown["demand.rate"] / 10.0)) * 10)
    if log.has("smoothed") and all(a == b for a, b in zip(log.col("room"), log.col("smoothed"))):
        seen.setdefault("auto.smooth", 0)
    target = [x for x in log.col("target") if x is not None]
    if target:
        seen.setdefault("auto.t", target[-1])
    settings = device.setdefault("settings", {})
    source = device.setdefault("source", {})
    changed = []
    for i, (k, v) in enumerate(seen.items()):
        old = settings.get(k)
        if old is not None and str(old) != str(v):
            changed.append("%s: %s -> %s" % (k, old, v))
        settings[k] = v
        source[k] = ("device " if i < read else "log ") + time.strftime("%Y-%m-%d", time.localtime(pulled))
    eff = [x for x in raw.get("effective") or [] if x is not None]
    if eff:
        device["target"] = eff[-1] / 100.0  # Effective target at the last sample, for the plant to run at
    device["host"] = host
    heatlog.write_json(device_path, device)

    index = heatlog.read_json(heatlog.index_path(), {}) or {}
    before = heatlog.all_logs()  # Includes the new file; the previous pull is the one before it
    prev = [l for l in before if l.name != name]
    index[name] = {"pulled": pulled, "host": host, "firmware": firmware, "build": build, "samples": n,
                   "interval": raw.get("interval"), "start": pulled - raw["ago"][0], "end": pulled - raw["ago"][-1]}
    heatlog.write_json(heatlog.index_path(), index)

    start, end = index[name]["start"], index[name]["end"]
    print("saved %s" % path)
    print("  %d samples every %s s, %s to %s (%.1f h)" % (n, raw.get("interval"), heatlog.stamp(start),
                                                         heatlog.stamp(end), (end - start) / 3600.0))
    print("  firmware %s built %s" % (firmware or "unknown", build or "unknown"))
    if read > 1:
        print("  %d heating settings read from the device (device.json)" % read)
    else:
        print("  HEATING SETTINGS NOT READ from the device (%s): %d are from the log, the rest are as last recorded. Ask "
              "the user for any that a conclusion depends on." % (
                  "its settings page asks for a password" if locked else "its settings page did not answer",
                  len(seen) - read))
    if prev:
        p = prev[-1]
        pf = (index.get(p.name) or {}).get("firmware")
        if start <= p.t[-1]:
            print("  continues %s (same boot): %.1f h of new data" % (p.name, (end - p.t[-1]) / 3600.0))
        else:
            print("  does not continue %s: the device restarted, or more time has passed than its log holds. %.1f h "
                  "between the logs is lost" % (p.name, (start - p.t[-1]) / 3600.0))
        if firmware and pf and firmware != pf:
            print("  FIRMWARE CHANGED since the last pull: %s -> %s" % (pf, firmware))
    for c in changed:
        print("  SETTING CHANGED since the last pull: %s" % c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
