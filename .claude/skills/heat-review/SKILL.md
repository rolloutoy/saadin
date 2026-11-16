---
name: heat-review
description: Pull the heat log from the Säädin device (saadin.local /graph.json), save it under graph-logs/, check it, test and refit the local house-and-unit model against it, then look for inconsistencies and for settings or firmware changes that would improve the heating control, trying each idea in simulation first. Use this whenever the user asks to pull or fetch the latest metrics, graph or log from the device, asks how the heating did (last night, this morning, since a change), asks whether to tune pid.kp, pid.ti, demand.*, defrost.* or the demand table, asks about flapping, overshoot, defrosts, defrost loops, COP or the heat deficit, wants the model updated or validated, or wants a control idea tried before it goes on the device. Use it even when they only say "check the graph" or "any new data?".
---

# Heat review

One pass over the heating controller's own record: get the data, make sure it can be trusted, make sure the model
can be trusted, and only then look for things to improve. The order matters. A sweep on a model that no longer
matches the house gives confident wrong answers, and an analysis of a log with a stuck sensor finds control problems
that are not there.

Everything runs from the repo root. Run the Python with `-I`: the logs are data from a device, and the scripts are
kept apart from them.

State lives in `graph-logs/`. It is git-ignored and stays that way: it is data from the user's own device and
home, and what was fitted from it. Never commit it, and keep figures from it out of anything that is committed
(code, docs, PR text); those go in the journal.

| File | What |
|---|---|
| `graph-<date>T<time>.json` | Logs exactly as the device sent them. Never edit or delete; the device forgets after 5 days or a restart |
| `index.json` | When each log was pulled, and the firmware it came from |
| `device.json` | The heating settings the device is running on, read from it at every pull, and where each value came from |
| `location.json` | Roughly where the house is, and the weather stations nearest to it |
| `weather.json` | Sun, wind and humidity by the hour from those stations |
| `model.json` | The model of the house and the unit, with its fit history |
| `events.json` | Heat sources and conditions the user has told of (a sauna, a fireplace, wind), which of them showed in the data, and which spans the user was asked about |
| `journal.md` | Dated notes: what each review found, decided, tried and rejected, and what is still open |

The scripts find `graph-logs/` beside the checkout they are in. Run from a git worktree, that is the worktree's own
empty one: set `HEATLOG_DIR` to the main checkout's `graph-logs` there (`HEATLOG_DIR=/path/to/saadin/graph-logs
python3 -I ...`). Nothing else keeps a copy of this directory, and the device does not keep its logs, so do not
clear it out.

## 1. Read the journal first

Read `graph-logs/journal.md` (the last few entries, and the "Tried and rejected" and "Open questions" lists). It
says what the device's settings were, what changed since, and which ideas were already tested and dropped. Without it
you will re-derive old findings and re-propose things that already failed in simulation.

If there is no journal yet (a first review, a fresh clone), start one from `references/journal-template.md` and
expect the model to be at its starting values: the first reviews are for collecting data and fitting, not for
recommending.

## 2. Pull

```
python3 -I .claude/skills/heat-review/scripts/pull.py       # --host other.local if the device is elsewhere
python3 -I .claude/skills/heat-review/scripts/weather.py    # sun, wind and humidity for the hours of the logs
```

`pull.py` only makes GET requests to the device. It prints whether the log continues the previous one or not, and
flags a firmware or settings change since the last pull. Act on those flags: data before and after a firmware change
or a settings change are two different experiments, so analyse them apart and say so.

If the device cannot be reached, say so plainly and ask whether to go on with the newest saved log. Do not present
old data as new.

`pull.py` also reads every heating setting from the device's own settings page (the target and its curve, the
demand, pid, idle, cold and defrost settings, the fans) and writes them to `device.json`. That file is therefore
what the device is running on now, not defaults and not memory, and it is what the plant runs on in step 7. Check
the count it prints. A `SETTING CHANGED` line is the user having changed something since the last pull: ask when,
because the log up to then ran on the old value (`report.py check` shows where the log disagrees with the settings
on record). If it prints `HEATING SETTINGS NOT READ` (a password on the settings page), only what the log gives away
is fresh: ask the user for anything else a conclusion depends on and record it in `device.json` with
`"source": "user <date>"`. Never POST to the device and never change a setting on it: proposing is this skill's job,
applying is the user's.

`weather.py` fetches from the Finnish Meteorological Institute's open data (opendata.fmi.fi, GET only, no key) what
the device cannot measure: global solar radiation, wind and humidity, by the hour. The outdoor temperature stays the
unit's own sensor. It needs to know roughly where the house is. If it says there is no location on record, ask the
user for the town, a post code or coordinates (a town is enough, and it leaves the machine only as a place name or
as station numbers), then:

```
python3 -I .claude/skills/heat-review/scripts/weather.py locate --place Jokioinen      # or --latlon 60.81,23.50
```

FMI does not know post codes, so turn one into its town. `locate` prints the two stations it chose and how far
they are; tell the user, because a radiation station far away has other clouds. If FMI cannot be reached, go on
without: the model then counts no sun or wind for the new hours, and says so.

## 3. Ask what happened in the house

The room reading is all the model has. Heat from anything but the unit, or an unusual loss, looks like a model error
or a controller fault unless it is on record, and `validate` in step 5 can only count what it has been told. So ask
before testing the model, not after it has failed.

Ask once, for the span that is new since the last review (`pull.py` printed it; name the days), with the question
tool if there is one. Ask about what moves a room by tenths of a degree:

- a sauna, a fireplace or stove, long use of the oven: which day and when it started, to the half hour
- other heating turned on or off (floor heating, a radiator, a second heat pump)
- many people in the house, or nobody for a day or more
- windows or doors open for long, airing, a change of the ventilation setting
- anything done to the unit or the sensor: target, fan, mode, a setting changed, the room sensor moved

Leave out what `events.json` says the house does not have, and do not ask again about a span already under
`asked`. Then record the answer in `graph-logs/events.json` (format in `references/model.md`):

- each burst of heat under `sources`, with its start. A loss goes there the same way under its own name ("airing");
  its figure just comes out negative
- the question itself under `asked`, with the span and the answer, also when the answer is "nothing unusual":
  that is information too, and it stops the same days being asked about twice
- standing facts ("no fireplace", "the sauna is electric and inside") under `noted`
- a changed setting in `device.json` as well

If nobody is there to answer, go on, and say in the report that the span was not asked about.

## 4. Check the data

```
python3 -I .claude/skills/heat-review/scripts/report.py check
```

Read every line that is not `ok`. Some are findings (a step against the room, a level 12% off the table), some
mean the log cannot carry the analysis (gaps, a room reading that stopped changing, time in `nosensor`). A `FAIL` on
the heat deficit books means the firmware's bookkeeping and this skill's understanding of it have parted: find out
which before trusting either (read `heatctl.c`; the firmware may simply have changed).

## 5. Validate the model, then refit it

```
python3 -I .claude/skills/heat-review/scripts/model.py validate   # the model AS IT STANDS against the new log
python3 -I .claude/skills/heat-review/scripts/model.py residual   # heat the model cannot explain, every two hours
python3 -I .claude/skills/heat-review/scripts/model.py fit        # refit on all logs; prints old -> new and saves
python3 -I .claude/skills/heat-review/scripts/model.py holdout    # fitted on the earlier part, scored on the rest
python3 -I .claude/skills/heat-review/scripts/sim.py compare      # the real controller on the plant, against the log
```

Do them in this order and report what each showed. `validate` is the only fully honest test there is, because the
new data was not in the last fit; once refitted, that chance is gone. Figures that moved by more than 20% in the
refit, and anything `OFF` in `compare`, are findings in their own right.

`fit` uses every saved log; `--hours N` keeps it to the latest N hours, for after a change that makes older data
another house or another unit. It takes about 20 s per 100 hours of data and `holdout` twice that, up to a few
minutes (beyond that they thin the data evenly), so give them a longer timeout once the logs span weeks. With too
little data `fit` says which figures it left at their starting values; `show` and the header of every sweep then
say so too. Believe them.

When `residual` still shows a run of hours well off zero, ask the user what happened at those times, naming the
times: the questions of step 3 were general, this one is specific. Record what they say in `events.json`, refit,
and check with `holdout` whether it helped; things that turn out not to show go under `noted` there, so they are not
asked about again.

The sun and the wind go into the fit when `weather.json` covers the data. They are two more terms, and more terms
always fit the data in hand better, so let `holdout` decide: run it with `--weather on` and `--weather off`, and
keep them only if the forecasts of held-back data are better with them. `fit --weather off` leaves them out and is
remembered until `fit --weather on`. Note the verdict in the journal.

`references/model.md` says what the model is, what it leaves out, how well each part is known, and how to judge and
repair it. Read it before drawing conclusions from a sweep, and whenever `validate`, `holdout` or `compare` is off.
The model is uneven: good within a defrost cycle, weak over hours. Say which of the two a conclusion rests on.

If `compare` is off in more than about one figure in five of the stretches that count, do not use sweeps to
recommend anything until that is understood. Say that the model cannot judge it yet. That is a legitimate result.

## 6. Look at what happened

```
python3 -I .claude/skills/heat-review/scripts/report.py cycles    # one row per defrost cycle
python3 -I .claude/skills/heat-review/scripts/report.py levels    # power and heat per level, against demand.table
python3 -I .claude/skills/heat-review/scripts/report.py steps     # every demand step with the rule's own numbers
python3 -I .claude/skills/heat-review/scripts/report.py defrost   # after-defrost profiles, frost per cycle, loops
python3 -I .claude/skills/heat-review/scripts/report.py trace --from "01-14 10:50" --to "01-14 12:30"
```

Start from what the user noticed, if they said anything, and find it in the data before explaining it: name the
times, trace them, and work out the mechanism from the columns. `references/method.md` lists the mechanisms already
understood (so you can recognise them quickly), the checks worth making every time, and the traps in reading these
numbers. Then look for what nobody asked about: compare this log's figures with the journal's earlier ones, and
follow up anything that changed.

Inconsistencies worth hunting, because each one has turned out to matter or could:

- the controller's books against the measured heat (delivery per cycle, steps against the room)
- the table against what the unit draws and gives at each level
- the model against the log (which way, when, and by how much)
- settings on record against what the log shows
- the heat estimate against physics (COP by level and fan speed, the same output at two fan speeds)
- the same weather on two days giving different heat need or defrost behaviour: `defrost` sets the humidity and
  the wind of every cycle beside it, and counts the short cycles by humidity

## 7. Try ideas before recommending them

```
python3 -I .claude/skills/heat-review/scripts/sim.py sweep --var "ti 45:TI=45" --grid KP=600,900,1350 --robust
python3 -I .claude/skills/heat-review/scripts/sim.py sweep --scen "-1,-600" --scen "-1,-600>-1,-100" --var "hold 5:HOLD=5"
python3 -I .claude/skills/heat-review/scripts/sim.py sweep --var "new rule:SRC=../saadin-wt/some-branch/ESP/main" --robust
python3 -I .claude/skills/heat-review/scripts/sim.py trace --scen "-1,-600" --var "hold 5:HOLD=5"
```

Add scenarios near the current weather, with the gain that `compare` found the model needs there. To judge how
settings follow a change in need, use a `,day` scenario (`--scen "-1,-900,day"`): it adds the fitted gain by time
of day, which is the disturbance the house really has, where a made-up ramp can favour whatever suits it. A settings
change is a `--var`; a firmware change is a `--var` with `SRC=` pointing at a worktree that has it (or a scratch
copy of `heatctl.c` for a first look), so the same plant judges both.

Hold every idea to the same bar, and say which of these it met:

1. It beats the device as it is in the scenarios it is meant for, by more than the noise (about 0.04 °C rms is
   within what the phase of the defrost cycle alone moves).
2. It is not clearly worse in the others, cold weather included.
3. It survives `--robust` (the harsher plants). The plant has few lags, so it flatters fast, strong settings.
4. The model is entitled to an opinion: the idea does not depend on something the model leaves out (defrost
   loops, fan speed, how frost responds to load, the sun and the wind hour by hour). If it does, it needs a device
   experiment, not a sweep; design one, with what to change, for how long, and which figure in the next log would
   confirm or refute it.

Most ideas fail this, and "no change, and here is why" is the common honest answer. Record the failures in the
journal's "Tried and rejected" so they are not tried again without new evidence.

## 8. Write the journal

Append a dated entry to `graph-logs/journal.md` (format at the top of that file): what was pulled, device and
firmware state, what the user said about the span, model changes and whether it validated, findings with their
numbers, what was tried in simulation and how it came out, recommendations given, and open questions. Update the
"Tried and rejected" and "Open questions" lists. Keep it factual and short; it is the memory the next review starts
from.

## 9. Report to the user

Lead with the answer to what they asked, or, with no specific question, with the one or two things most worth
knowing. Then, in this order and only as far as each has something to say:

- **Data:** what was pulled (span, firmware, weather), and anything that limits it.
- **Model:** did it validate, what moved in the refit, can it judge the ideas below.
- **What happened:** findings, each with the times and numbers behind it.
- **What to change:** split into settings to change now, firmware changes worth a PR, and experiments or more data
  needed. For each: the expected gain, how sure, and what it rests on (log, model, or both).
- **Not changed and why:** ideas checked and dropped, briefly.

State what was measured, what was simulated and what is inference, separately. Nothing in this skill flashes
firmware or changes the device; if a firmware change is worth making, propose it, and if the user wants it, it
follows the repo's rules (worktree, failing test first, PR, no merge without approval, S21 code untouched).

## When the tools break

- **The plant does not build:** the firmware's `heatctl_params_t` or `heatctl_in_t` changed (the plant checks
  their sizes, and `make -C ESP test` builds it, so this should show when the firmware changes and not here). Fix
  `scripts/plant.c` to follow (it is a harness): give each new field its value, pass a new setting through
  `SETTING_ENV` in `sim.py`, update the sizes, then rerun `compare` to see that nothing else moved.
- **New columns in `/graph.json`, or renamed ones:** `scripts/heatlog.py` loads whatever columns exist; add the
  scaling or an alias there, and use the column in `report.py` where it helps.
- **The books check fails after a firmware change:** the deficit rule changed; update `books()` and `step_rows()`
  in `report.py` from `heatctl.c`, and note it in `references/method.md`.
- **FMI's replies change, or a station closes:** `weather.py locate` again picks the nearest stations still in
  use; the names of what is fetched are at the top of `scripts/weather.py`.

Improve the scripts when an analysis turns out to be worth repeating. A table you built by hand twice belongs in
`report.py`. After changing a script, run its tests, which make up their own logs and touch neither `graph-logs/`
nor the network, and add one for what you changed:

```
python3 -I .claude/skills/heat-review/scripts/selftest.py
```
