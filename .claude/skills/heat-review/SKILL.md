---
name: heat-review
description: Pull the heat log from the Säädin device (saadin.local /graph.json), save it under graph-logs/, check it, test and refit the local house-and-unit model against it, then look for inconsistencies and for settings or firmware changes that would improve the heating control, trying each idea in simulation first. Use this whenever the user asks to pull or fetch the latest metrics, graph or log from the device, asks how the heating did (last night, this morning, since a change), asks whether to tune pid.kp, pid.ti, demand.*, defrost.* or the demand table, asks about flapping, overshoot, defrosts, defrost loops, COP or the heat deficit, wants the model updated or validated, or wants a control idea tried before it goes on the device. Use it even when they only say "check the graph" or "any new data?".
---

# Heat review

One pass over the heating controller's own record: get the data, make sure it can be trusted, make sure the model
can be trusted, and only then look for things to improve. The order matters. A sweep on a model that no longer
matches the house gives confident wrong answers, and an analysis of a log with a stuck sensor finds control problems
that are not there.

Everything runs from the repo root. `S` below is `.claude/skills/heat-review/scripts`. Run the Python with `-I`:
the logs are data from a device, and the scripts are kept apart from them.

State lives in `graph-logs/`. It is git-ignored and stays that way: it is data from the user's own device and
home, and what was fitted from it. Never commit it, and keep figures from it out of anything that is committed
(code, docs, PR text); those go in the journal.

| File | What |
|---|---|
| `graph-<date>T<time>.json` | Logs exactly as the device sent them. Never edit or delete; the device forgets after 5 days or a restart |
| `index.json` | When each log was pulled, and the firmware it came from |
| `device.json` | The device's heating settings as far as known, and where each value came from |
| `model.json` | The model of the house and the unit, with its fit history |
| `events.json` | Heat sources and conditions the user has told of (a fireplace, wind), and which of them showed in the data |
| `journal.md` | Dated notes: what each review found, decided, tried and rejected, and what is still open |

## 1. Read the journal first

Read `graph-logs/journal.md` (the last few entries, and the "Tried and rejected" and "Open questions" lists). It
says what the device's settings were, what changed since, and which ideas were already tested and dropped. Without it
you will re-derive old findings and re-propose things that already failed in simulation.

## 2. Pull

```
python3 -I $S/pull.py            # --host other.local if the device is elsewhere
```

It only makes GET requests. It prints whether the log continues the previous one or the device restarted, and flags
a firmware or settings change since the last pull. Act on those flags: data before and after a firmware change or a
settings change are two different experiments, so analyse them apart and say so.

If the device cannot be reached, say so plainly and ask whether to go on with the newest saved log. Do not present
old data as new.

`device.json` only knows what the device shows without a POST (a few settings on the first settings page, kp and ti
from the log, and what the log gives away: hold, rate, smoothing). For anything else that a conclusion depends on
(for example `defrost.holdmax`, `demand.full`, `defrost.watchdog`), ask the user, then record it in `device.json`
with `"source": "user <date>"`. Never POST to the device and never change a setting on it: proposing is this
skill's job, applying is the user's.

## 3. Check the data

```
python3 -I $S/report.py check
```

Read every line that is not `ok`. Some are findings (a step against the room, a level 12% off the table), some
mean the log cannot carry the analysis (gaps, a room reading that stopped changing, time in `nosensor`). A `FAIL` on
the heat deficit books means the firmware's bookkeeping and this skill's understanding of it have parted: find out
which before trusting either (read `heatctl.c`; the firmware may simply have changed).

## 4. Validate the model, then refit it

```
python3 -I $S/model.py validate      # the model AS IT STANDS against the new log, before any refit
python3 -I $S/model.py residual      # heat the model cannot explain, every two hours along the log
python3 -I $S/model.py fit           # refit on all logs; prints old -> new and saves
python3 -I $S/model.py holdout       # the house fitted on the earlier part, scored on the part held back
python3 -I $S/sim.py compare         # closed loop: the real controller on the plant, against the log
```

Do them in this order and report what each showed. `validate` is the only fully honest test there is, because the
new data was not in the last fit; once refitted, that chance is gone. Figures that moved by more than 20% in the
refit, and anything `OFF` in `compare`, are findings in their own right.

The room reading is all the fit has, so heat from anything but the unit (a sauna, a fireplace, an oven, guests) or
an unusual loss (an open window, a storm) bends the house figures unless the fit is told. When `residual` shows a
run of hours well off zero, ask the user what happened at those times, naming the times. Record what they say in
`graph-logs/events.json` (format in `references/model.md`), refit, and check with `holdout` whether it helped;
things that turn out not to show go under `noted` there, so they are not asked about again.

`references/model.md` says what the model is, what it leaves out, how well each part is known, and how to judge and
repair it. Read it before drawing conclusions from a sweep, and whenever `validate`, `holdout` or `compare` is off.
The model is uneven: good within a defrost cycle, weak over hours. Say which of the two a conclusion rests on.

If `compare` is off in more than about one figure in five of the stretches that count, do not use sweeps to
recommend anything until that is understood. Say that the model cannot judge it yet. That is a legitimate result.

## 5. Look at what happened

```
python3 -I $S/report.py cycles       # one row per defrost cycle
python3 -I $S/report.py levels       # power and heat per level over all logs, against demand.table
python3 -I $S/report.py steps        # every demand step with the rule's own numbers
python3 -I $S/report.py defrost      # after-defrost profiles, frost per cycle, short cycles (defrost loops)
python3 -I $S/report.py trace --from "01-14 10:50" --to "01-14 12:30"     # the samples themselves
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
- the same weather on two days giving different heat need or defrost behaviour

## 6. Try ideas before recommending them

```
python3 -I $S/sim.py sweep --var "ti 45:TI=45" --grid KP=600,900,1350 --robust
python3 -I $S/sim.py sweep --scen "-1,-600" --scen "-1,-600>-1,-100" --var "hold 5:HOLD=5"
python3 -I $S/sim.py sweep --var "new rule:SRC=../saadin-wt/some-branch/ESP/main" --robust
python3 -I $S/sim.py trace --scen "-1,-600" --var "hold 5:HOLD=5"
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
   loops, fan speed, how frost responds to load). If it does, it needs a device experiment, not a sweep; design
   one, with what to change, for how long, and which figure in the next log would confirm or refute it.

Most ideas fail this, and "no change, and here is why" is the common honest answer. Record the failures in the
journal's "Tried and rejected" so they are not tried again without new evidence.

## 7. Write the journal

Append a dated entry to `graph-logs/journal.md` (format at the top of that file): what was pulled, device and
firmware state, model changes and whether it validated, findings with their numbers, what was tried in simulation
and how it came out, recommendations given, and open questions. Update the "Tried and rejected" and "Open
questions" lists. Keep it factual and short; it is the memory the next review starts from.

## 8. Report to the user

Lead with the answer to what they asked, or, with no specific question, with the one or two things most worth
knowing. Then, in this order and only as far as each has something to say:

- **Data:** what was pulled (span, firmware), and anything that limits it.
- **Model:** did it validate, what moved in the refit, can it judge the ideas below.
- **What happened:** findings, each with the times and numbers behind it.
- **What to change:** split into settings to change now, firmware changes worth a PR, and experiments or more data
  needed. For each: the expected gain, how sure, and what it rests on (log, model, or both).
- **Not changed and why:** ideas checked and dropped, briefly.

State what was measured, what was simulated and what is inference, separately. Nothing in this skill flashes
firmware or changes the device; if a firmware change is worth making, propose it, and if the user wants it, it
follows the repo's rules (worktree, failing test first, PR, no merge without approval, S21 code untouched).

## When the tools break

- **The plant does not build:** the firmware's `heatctl_params_t` or `heatctl_in_t` changed. Fix `scripts/plant.c`
  to follow (it is a harness), then rerun `compare` to see that nothing else moved.
- **New columns in `/graph.json`, or renamed ones:** `scripts/heatlog.py` loads whatever columns exist; add the
  scaling or an alias there, and use the column in `report.py` where it helps.
- **The books check fails after a firmware change:** the deficit rule changed; update `books()` and `step_rows()`
  in `report.py` from `heatctl.c`, and note it in `references/method.md`.

Improve the scripts when an analysis turns out to be worth repeating. A table you built by hand twice belongs in
`report.py`.
