#!/usr/bin/env python3
"""Pull the heat log from the device and save it in graph-logs/.

Usage: python3 -I pull.py [--host saadin.local]

Only GET requests are made: /graph.json (the log), /graph (firmware id in the footer) and /revk-settings (the first
settings page, from which a fixed list of heating settings is read; nothing else on that page is kept).
Writes graph-logs/graph-<local time>.json unchanged from the device, and notes the pull in graph-logs/index.json
and what is known of the device's settings in graph-logs/device.json.
"""
import argparse
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

# Heating settings on the first settings page, by the page's field id -> setting name
PAGE_SETTINGS = {"autocurve": "auto.curve", "demandtable": "demand.table", "demandcop": "demand.cop",
                 "fanflow": "fan.flow", "demandfanmap": "demand.fanmap", "fanhold": "fan.hold"}


def get(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


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

    seen = {}
    try:
        page = get("http://%s/revk-settings?_page=0" % host, 20).decode("utf-8", "replace")
        for field, setting in PAGE_SETTINGS.items():
            m = re.search(r'<input id="%s"[^>]*\svalue="([^"]*)"' % field, page)
            if m:
                seen[setting] = html.unescape(m.group(1))
    except Exception:
        pass
    for col, setting in (("kp", "pid.kp"), ("ti", "pid.ti")):  # As in use at the last sample (in demand control)
        v = [x for x in raw.get(col) or [] if x is not None]
        if v and raw.get("state") and raw["state"][-1] == heatlog.DEMAND:
            seen[setting] = v[-1]
    if raw.get("table"):
        seen["demand.table"] = raw["table"]
    # Settings the device does not show without a POST, as far as the log itself gives them away
    log = heatlog.Log(path, {name: {"pulled": pulled}})
    shown = report.inferred(log)
    if "defrost.hold" in shown:
        seen["defrost.hold"] = int(round(shown["defrost.hold"]))  # Minutes; a hold reads a sample short
    if "demand.rate" in shown:
        seen["demand.rate"] = int(round(shown["demand.rate"] / 10.0)) * 10
    if log.has("smoothed") and all(a == b for a, b in zip(log.col("room"), log.col("smoothed"))):
        seen["auto.smooth"] = 0
    settings = device.setdefault("settings", {})
    source = device.setdefault("source", {})
    changed = []
    for k, v in seen.items():
        old = settings.get(k)
        if old is not None and str(old) != str(v):
            changed.append("%s: %s -> %s" % (k, old, v))
        settings[k] = v
        source[k] = ("log " if k in ("defrost.hold", "demand.rate", "auto.smooth") else "device ") + time.strftime(
            "%Y-%m-%d", time.localtime(pulled))
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
    if prev:
        p = prev[-1]
        pf = (index.get(p.name) or {}).get("firmware")
        if start <= p.t[-1]:
            print("  continues %s (same boot): %.1f h of new data" % (p.name, (end - p.t[-1]) / 3600.0))
        else:
            print("  the device restarted since %s: %.1f h between the logs is lost" % (p.name, (start - p.t[-1]) / 3600.0))
        if firmware and pf and firmware != pf:
            print("  FIRMWARE CHANGED since the last pull: %s -> %s" % (pf, firmware))
    for c in changed:
        print("  SETTING CHANGED since the last pull: %s" % c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
