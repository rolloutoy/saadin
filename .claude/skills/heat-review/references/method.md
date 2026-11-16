# Reading the heat log

What the columns mean, the mechanisms already understood, and the traps. The firmware is the authority: when
something here disagrees with `ESP/main/heatctl.c` or `Manuals/Heating.md`, the firmware changed, so read it and
fix this file.

## Contents

- The quantities
- Mechanisms seen so far
- Checks worth making every time
- Traps in the numbers

## The quantities

| Column | Meaning |
|---|---|
| `room`, `effective` | Room reading and the target in force (`target` plus the `auto.curve` offset by outdoor temperature) |
| `heatw` | Heat request, W of heat: `pid.kp` x (effective - room) + `integral` |
| `integral` | The slow part of the request. Settled, it is the house's mean heat need |
| `heattarget` | Request plus the make-up for defrosts. The make-up is the heat lost in defrosts and the hold, paid back over about two hours, so with regular defrosts it is a nearly constant few hundred watts |
| `deficit` | Running total of target minus estimated output, Wh. Demand steps to keep it near zero. It has no time window and never fades |
| `heatout` | Estimated heat output: air flow at the fan speed (`fan.flow`) warmed from intake to liquid temperature. An estimate, never checked against a meter |
| `scale` | The one learnt figure for heat per watt of `demand.table`, smoothed over 30 min at the level in use |
| `demand`, `quiet` | The level sent. `100` with quiet outdoor is "100q"; plain `100` has quiet off |
| `sub` | `defrost` (liquid below `defrost.enter`), `hold` (the minutes after), `rest` |

A demand step is due once the deficit reaches half of what the next level would over-give in `demand.rate`, and
only from a level that itself gives too little (up) or too much (down). `report.py steps` rebuilds this for every
step, with the `demand.rate` on record in `device.json`.

## Mechanisms seen so far

Recognise these before looking for something new. They come from the firmware's rules and from the unit, so they
should hold on any device; how large each is in this house is in the journal (`graph-logs/journal.md`, "Standing
figures"), with the dates they were established.

- **The gap between the top levels.** With the default table the level below 100% and 100% with quiet outdoor
  ("100q") differ by about a factor of two in heat, with nothing between. A need in the gap is met by one 100q burst
  per defrost cycle, at least `demand.rate` long. This sets most of the room's ripple in cold weather, and no setting
  removes it.
- **The proportional part follows the defrost sawtooth.** Each defrost drops the room, so the request swings by
  about `pid.kp` times half that drop. Where the levels are close together in heat, demand runs up after each defrost
  and down before the next. It looks like flapping and has had no measurable cost.
- **One scale for all levels.** The controller multiplies the table by one learnt heat-per-watt figure, but the unit's
  COP falls at the 100% levels. The rule therefore expects more from a step up to 100q than comes, and less from
  the step back. The totals are not hurt (the deficit counts measured heat); only the timing of steps is.
- **Output collapses before a defrost is seen.** The unit winds down a minute or two before the liquid temperature
  falls, and that shortfall is booked as ordinary deficit. It can cause a pointless step up just before a defrost.
- **The hold pins the level.** For `defrost.hold` after a defrost demand cannot rise. The room is lowest a few minutes
  after the defrost ends and barely recovers until the hold is over.
- **The last 100q burst as the need falls.** When the need drops below what the level under 100q gives, one more
  minimum-length burst can arrive with the room already over target.
- **The make-up store starts empty.** After a fresh start the first few cycles deliver under the request while it
  fills.
- **Surplus is dropped at the ends of the table.** At the lowest and the top level the firmware caps the deficit, so
  the books do not close in those cycles. By design.
- **The integral after heating was off.** In firmware up to abcbb98, leaving heating off (idle) set the integral to
  the heat of the lowest level (`enter_demand()`), whatever it was. After a burst of other heat took the room over
  target and heating went off, the room then sat under target for hours while the integral climbed back. Later
  firmware keeps the integral. Look at what the integral does after every `idle` in a log. A restart did the same
  before the state was kept over restarts.
- **Short defrost cycles.** Around 0 degC the unit can defrost again soon after a defrost, after a fraction of the
  usual heat, at a much lower cycle COP. `report.py defrost` counts them and sets them against the load after the
  defrost, the weather (humidity and wind too, once `weather.py` has pulled) and the length of the defrost. The
  cause is not established.

## Checks worth making every time

1. Mean room error per defrost cycle (`cycles`). Within about 0.1 degC is the controller doing its job; a run of
   cycles off one way is a change in need being tracked, or a limit being hit.
2. Delivery (`check`): output against request per cycle. More than a few percent off in settled running means the
   account could not deliver (a table end, a burst quantum) or the estimate moved (fan speed change).
3. Defrosts (`defrost`): interval, heat per cycle, short cycles and what preceded them, and whether they keep to
   humid hours (the humidity column is there once `weather.py` has pulled).
4. Efficiency: COP per cycle with the defrost counted in, by weather and by time at 100q.
5. Levels (`levels`): settled power and heat against the table, and whether the heat per table watt is the same
   across levels, fan speeds and outdoor temperatures.
6. Steps (`steps`): steps against the room, steps just before a defrost, predictions against what came.
7. What changed against the last review: weather, settings, firmware, and each of the figures above.

## Traps in the numbers

- **Everything in watts of heat rests on `heatout`.** A bias common to all levels is absorbed by the integral and
  does no harm. A bias that differs between levels or fan speeds shifts what the account delivers while its books
  still close. The same compressor frequency at two fan speeds giving different "heat" is the estimate, not the unit.
- **The outdoor reading is the unit's own sensor by the coil.** It reads in half degrees, jumps in a defrost, and
  is frozen by the firmware through defrost and hold.
- **Cycle figures need whole cycles.** A window cut mid-cycle gives a wrong mean. Use `cycles`.
- **Logs from firmware before the heat account (no `deficit` column) have `heatw`, `integral` and `kp` in
  electrical watts.** Their room, power and heat columns are still good.
- **Internal gains are real and large.** A house gets a few hundred watts from its people and appliances, more by
  day than by night, and a burst from cooking, a sauna or a fire. `model.py fit` puts figures on the daily pattern and
  on the sources the user has told of. Do not read the resulting changes in heat need as a controller fault, and do
  not tune to one day. `model.py residual` shows what is left over; ask the user about hours that stand out.
- **A stretch the house model cannot explain is not yet a fact about the house.** Check first what the controller
  was doing: the two largest "unexplained" stretches so far were the hours after it had under-heated (an integral
  reset), where the model's response over hours is at its weakest. Trace the stretch before asking the user about
  windows and weather.
- **The weather is a station's, not the house's.** Sun, wind and humidity come from FMI stations that may be tens
  of kilometres away (`weather.py show` says how far). Humidity near 100% with the outdoor reading around zero is
  frost weather anywhere nearby; one hour of sun or a gust is not evidence of anything at the house. Compare days
  and runs of hours.
- **In simulation, differences under about 0.04 degC rms are noise**: the plant settles into a limit cycle, and
  which phase it locks to moves the figures by that much.
- **The plant flatters strong, fast settings**, because it has little lag. That is what `--robust` is for.
