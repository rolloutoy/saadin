# Heat review journal

Kept by the heat-review skill (`.claude/skills/heat-review`). Newest entry first, under the standing lists.
An entry has: **Pulled** (log, span, firmware, weather), **Device** (settings in force, changes since last time),
**House** (what the user said happened in the span), **Model** (validation result, what the refit moved,
closed-loop compare), **Findings** (with times and numbers), **Tried** (simulated ideas and how they came out),
**Recommended**, **Open**.

This file is data from the user's own device and home. It lives in `graph-logs/`, which is git-ignored: never
commit it, and keep its figures out of code, docs and PR text.

## Device state

- Firmware: (id and build time from `pull.py`), running since (start of the newest log).
- Settings: (those that differ from the firmware's defaults, from `device.json`, which `pull.py` reads from the
  device at every pull; the target and since when).
- Changed since the last review: (each `SETTING CHANGED` line, and when the user says it was changed).

## Standing figures

What this house and this unit do. `references/method.md` describes the mechanisms; how large each is here goes in
this list, with the date it was established. Update a line when a review moves it.

- Levels: (settled power and heat per level against `demand.table`, by fan speed and weather)
- A defrost: (how long, how often, what it does to the room)
- Short defrost cycles: (how many, in what weather and humidity)
- Gains: (by time of day; each told-of source; sun and wind if the fit keeps them)
- House: (the fitted figures, and how well held-back data is followed)
- Weather: (the stations and how far they are; whether `holdout` was better with sun and wind)

## Tried and rejected

Do not propose these again without new evidence. One line each: what, in which scenarios, how it came out, date.

## Open questions

What the next review should look at, and what data or experiment would settle each.

## YYYY-MM-DD: (the main thing this review found)

**Pulled:**

**Device:**

**House:**

**Model:**

**Findings:**

**Tried:**

**Recommended:**

**Open:**
