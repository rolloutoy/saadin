# The model

`graph-logs/model.json` describes the house and the unit; `scripts/plant.c` runs the firmware's own `heatctl.c`
against it, one second at a time. It exists to compare control settings and rules with each other, not to predict
tomorrow's room temperature.

## Contents

- What is in it and where each figure comes from
- Heat the log does not record
- The weather
- What it leaves out
- Judging it
- When it is off
- Trying firmware changes

## What is in it and where each figure comes from

**House, two nodes.** The room air takes the unit's heat and exchanges with a mass, which loses to outdoors.

| Figure | Fitted on | How well known |
|---|---|---|
| `air_J_K`, `air_mass_W_K`, `defrost.room_W` | The defrosts as step tests: 15 min before to 60 min after each, every window with its own mass temperature | The response is well known once there are a few dozen defrosts. The three figures trade off against each other along a flat valley, so quote the response (how well unseen defrosts are followed), not one figure |
| `gains.heat_scale_by_fan` | Same windows. A scale on the heat estimate for each fan setting but the commonest | Weakly, and only for fan settings with many defrosts behind them |
| `mass_J_K`, `mass_out_W_K` | Rolling forecasts: the mass temperature set from four hours, the room then forecast twelve hours on | Poorly until there are many days in varied weather; see "Judging it" |
| `gains.daily_W` | With the slow fit. Extra heat in the house by time of day, against 00-06 as zero | Plausible after a few days; better with more |
| `gains.sources_W` | With the slow fit. Each heat source told of in `events.json`, by hours since its start | As good as the times given |
| `gains.weather` | With the slow fit, when `weather.json` covers the data. Heat from the sun per W/m2 of global radiation, and extra loss per m/s of wind | Not until `holdout` is better with them than without; see "The weather" |

The fit is in two stages because one fit of everything lets the slow drift over days decide the fast figures too,
and then the plant's swing per defrost cycle comes out too small. The slow stage scores forecasts and not one long
run because in a long run the mass soaks up any heat nobody logged, and the loss figure with it.

**Unit.** Heat and power per watt of a fixed reference table for the levels up to 95%, and heat and power of 100q
and plain 100, each as a line in outdoor temperature fitted on settled running (`report.py levels`). A slope is only
refitted when its data spans 4 degC, and a line is held flat beyond 3 degC outside its data. Ramp rates come from
steps up to 100q.

**Defrost.** One comes once `frost_Wh` of heat has been given since the last (the median over normal cycles),
lasts `seconds` at `power_W` of electricity, and the output falls `lead_s` before the controller can see it.

## Heat the log does not record

The room reading is all the fit has, so heat from anything but the unit bends the house figures unless the fit is
told. `graph-logs/events.json` holds what the user has said:

```json
{"sources": [{"name": "fireplace", "start": "2026-01-10 18:30", "told": "user 2026-01-12: lit about half past six"}],
 "asked":   [{"span": "2026-01-09 08:00 to 2026-01-12 09:00", "on": "2026-01-12",
              "answer": "fireplace on Saturday evening, otherwise nothing unusual"}],
 "noted":   [{"what": "strong wind", "when": "2026-01-08 and 09", "result": "no extra loss found"},
             {"what": "no sauna in the house", "when": "standing", "result": "do not ask"}]}
```

- A **source** gets three amplitudes (0-2 h, 2-4 h, 4-7 h after its start), shared by all events of the same name.
  Give the start to the half hour; when the user is unsure, try a few and keep the one the fit likes (lowest rms).
  A loss (a window left open, airing) is a source too, under its own name: its amplitudes come out negative. A
  source needs a few events before its figure means much, and `validate` counts a new event only if the model
  already has a figure for that name.
- **Asked** is the record of the questions of step 3 of the skill: which span, when, and what the user said, also
  when that was "nothing". The fit does not read it. It is there so that no span is asked about twice, and so
  that a later reader can tell "nothing happened" from "nobody asked".
- **Noted** is for things told that turned out not to show (so they are not asked about or tried again), and for
  standing facts ("no sun to speak of in November").
- `model.py residual` lays the heat the model cannot explain along the log in two-hour steps. A run of hours well
  off zero is the cue to ask the user what happened then. Ask about specific times; people remember "Sunday
  evening" better than "any heat sources?".

To try a new kind of term (rain, a scale on the estimate at some level), add a channel next to `ch_day`,
`ch_source` and `ch_sun` in `model.py` and judge it with `holdout`.

## The weather

`scripts/weather.py` keeps, in `graph-logs/weather.json`, what the device cannot measure, by the hour, from the
Finnish Meteorological Institute's open data: global solar radiation from the nearest radiation station, and wind
and humidity from the nearest weather station (`graph-logs/location.json` says which, and how far they are). The
outdoor temperature in the model stays the unit's own sensor; the station's temperature is kept only because its
humidity is relative to it.

- **Sun** is a term of the slow fit: watts into the room per W/m2 of global radiation. Without it the sun sits in
  the gain by time of day, which then differs between a clear day and a grey one.
- **Wind** is a term of the slow fit: extra loss per m/s over the mean wind of the fit, per degree between room and
  outdoors.
- **Humidity** is not in the model. `report.py defrost` sets it beside every defrost cycle and counts the short
  cycles by it, because frost is the one thing here that humidity could explain.

Both terms are in a fit only when the weather on record covers nine tenths of its data, and each is one more thing
that can soak up heat nobody logged. Keep them only when `model.py holdout --weather on` forecasts the held-back
data better than `--weather off`; `fit --weather off` leaves them out until `fit --weather on`. Expect the sun to
show from late winter on and hardly at all around midwinter, and expect a few days of data to say little either
way.

A station is not the house. There are few radiation stations, so the nearest may be far enough to have other
clouds: its days are worth more than its hours. Wind is measured on open ground, ten metres up, and a house among
trees or other houses gets less of it. The values are hourly means, stamped by the end of the hour.

## What it leaves out

Be explicit about these whenever a conclusion leans on them.

- **Defrost loops.** The plant defrosts on heat given and nothing else. It has no short cycles, so it cannot judge
  anything meant to prevent or exploit them, and its COP is too high for weather where they occur.
- **How frost responds to load, weather and humidity.** "A defrost per so many Wh" is a fit to the logs, with a wide
  spread.
- **Fan speed.** The unit's figures mix whatever fan speeds the logs had. The fan map changes fan speed with
  outdoor temperature, so the outdoor slope partly stands for it. The plant gives the house the estimated heat
  as it is, without the per-fan scale.
- **Full-power (cold) mode.** Below `cold.enter` the firmware controls by setpoint; the plant just gives all it
  has. Results below about -13 degC mean little.
- **Sensor placement and lags**, except through the `--robust` variants.
- **Heating off (idle).** It happens in the plant, but the unit's behaviour at restart is not fitted.
- **The sun and the wind, hour by hour.** They are in the fit of the house, so that they do not bend its figures,
  but the plant runs in still, dark weather: a scenario's gain stands for all of it, and a `,day` scenario carries
  the gain by time of day only.

## Judging it

In rising order of what they prove:

1. `model.py validate` on a new log, before refitting. Rolling forecasts of the new data at 1, 3, 6 and 12 h,
   next to "no model" (the room stays as it was). The controller holds the room nearly still, so "no model" is hard
   to beat at long horizons; a model that knows the heat input must still beat it to be worth using there. A mean
   that drifts one way with the horizon is heat or loss the model does not have.
2. `model.py holdout`. The same on data cut out of the fit. Use it whenever the structure or the fit changes.
3. `model.py fit`: what moved. Figures that keep moving by more than 20% from fit to fit are not pinned down.
4. `sim.py compare`: the closed loop. For each stretch of the log the plant runs in the same weather with the
   device's settings, with a gain chosen so that it needs the same heat. Defrost interval, time at 100q, swing, rms
   error, COP and demand changes should then match. Stretches that were not settled or that had defrost loops are
   shown and not counted. The "gain needed" shows how far the level of need is off at that time.

How the model stands on each of these (what it can be trusted for, and what not yet) is recorded in the journal
after every fit. The pattern to expect while the data is days and not weeks: good within a defrost cycle; uncertain
over hours, where the misses gather in the recoveries after the controller under-heated (a restart, heating coming
back from off), because those are the only times the room moves far. Trust it for what is decided within a defrost
cycle, take the direction but not the size of anything decided over hours, and set the level of need per scenario
with a gain.

## When it is off

- **Swing or rms too small in `compare`:** the fast figures are too soft. Check that `fit` reports "fast, N
  defrosts" with a good N and under about 0.1 degC through the defrosts.
- **Time at 100q or COP off at one temperature:** the unit's lines. Look at `report.py levels` for that band and fan
  speed; a fan speed the model has little data for is the usual cause.
- **Defrost interval off:** frost per cycle differs in this weather, or there are loops. Not fixable by fitting; state
  it as a limit.
- **Forecasts drift one way, or gain needed is large:** unlogged heat or loss. Run `residual`, ask the user about
  the hours that stand out, record the answer in `events.json`, refit, and see whether `holdout` improved. If it did
  not, keep the entry under `noted` with the result.
- **Everything moved after a firmware or settings change:** fit and compare on data since the change only
  (`model.py fit --hours N`).

Changing the model's structure (a third node, a fan term, a rule for loops) is fair when the logs clearly ask for
it. Do it in `model.py` and `plant.c` together, keep `holdout` and `compare` as the judges, and note it here and in
the journal. More terms always fit the data in hand better; only held-back data says whether they are real.

## Trying firmware changes

`sim.py sweep --var "label:SRC=<dir with heatctl.c and heatctl.h>"` builds the plant against another source tree,
so a change on a branch can be judged before it is flashed. `DEFS="-DNAME"` passes compiler flags, for a scratch copy
with the change behind an `#ifdef`. Builds are cached in `graph-logs/.build/`.

The plant's controller runs on the device's own settings: `pull.py` reads them from the device into `device.json`,
and `sim.py` passes on those that do something in the plant (`SETTING_ENV` there), the target and its curve
(`auto.t`, `auto.curve`) among them, so the target moves with the weather of a scenario as it does on the device. A
variant overrides them, and nothing comes from the shell's environment. A setting that `device.json` lacks is at the
firmware's default, and `demand.start` is always the plant's own (see `plant.c`). In `compare` the target is the
one set in each stretch of the log; the other settings are those of the last pull, so a setting changed since the
log was recorded makes that comparison one of two different controllers.

A scenario ending in `,day` (for example `-1,-900,day`) adds the fitted gain by time of day. That is the
disturbance the house really has, so prefer it to made-up ramps when judging how settings track a change in need.

A change judged this way still needs the repo's process to become firmware: a worktree, a failing host test first,
`make s3` and `make pico` clean, a PR. And the plant's verdict is only as good as the list above allows.
