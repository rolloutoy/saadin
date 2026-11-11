# Heating control

Säädin has its own heating controller. It replaces *Faikout auto*. It keeps a room at a target temperature by limiting the heat pump's power ("demand"), so the unit runs steadily at low power instead of switching on and off.

The controller only works in heat mode. It never turns the unit on or off, and does nothing in cool, dry, fan or auto mode.

The design, with the reasons behind each choice, is in [docs/heat-control.md](../docs/heat-control.md).

## What you need

- **A room temperature reading.** Use a BLE sensor (see [BLE sensors](BLE.md)) or send the temperature over MQTT (see [Room temperature over MQTT](#room-temperature-over-mqtt)). The controller does not use the unit's own temperature sensor, because the unit reads too high when it heats hard.
- **A unit that supports demand control.** If the main web page has a *Demand* control, the unit supports it. Without demand control, the controller can only start and stop heating with the setpoint, so the room temperature swings more.
- **The liquid temperature** (`liquid` in the status), to detect defrosts. Without it, defrosts are not detected.
- **The outdoor temperature** (`outside` in the status), for the target curve and for the cold-weather mode. Without it, the target has no curve offset and cold-weather mode never starts.

## Turning it on

1. Turn the unit on in heat mode.
2. On the web page, set **Target** to the room temperature you want, and turn on **Heating control**.

You can also use MQTT, for example `setting/GuestAC {"autoe":true,"autot":21.5}`, or the Home Assistant "Heating control" switch and "Heating target" number.

`auto.e` is off by default. The controller runs only when `auto.e` is on, `auto.t` is set, and the unit is on in heat mode.

While the controller runs, the web page greys out *Set*, *Demand* and *Quiet outdoor*. A status line under the target shows what the controller is doing.

## What it does

The controller is always in one of these states. The web status line and the `ctl` status field show the current one.

|State|Web status line|What the unit gets|When|
|-----|---------------|------------------|----|
|`off`|*(empty, or "Not running")*|Nothing|`auto.e` off, `auto.t` not set, unit off, or not in heat mode|
|`demand`|Demand 60% QO · target 21.3 · heat 350 W|Setpoint 30 °C, demand limited|Normal heating|
|`idle`|Heating off · target 21.3|Setpoint 16 °C, lowest demand|The room is more than 0.5 °C above the target|
|`cold`|Cold · setpoint 24.5 · target 21.3|Demand 100%, the setpoint adjusted|Below −15 °C outside|
|`nosensor`|No room reading · unit thermostat at 21.0|Setpoint = target, demand 100%|No room reading for 30 minutes|

QO is *quiet outdoor*. The controller turns it on at every demand level except plain 100%.

### Demand

The controller sends the unit a high setpoint (`demand.set`, 30 °C), so the unit always wants to heat. It then limits the unit's power with demand. A PI loop (proportional plus integral) works out how much heat the room needs, in watts. The demand table (`demand.table`) turns those watts into a demand level.

So in this state the web page shows *Set* 30 °C. This is expected.

Demand changes at most once every 10 minutes (`demand.rate`), but one change can skip several levels.

### Heating off (idle)

When the room is more than `idle.over` (0.5 °C) above the target, demand is already at its lowest level, and heating has run for at least `demand.minrun` (30 minutes), the controller sets the setpoint to `idle.temp` (16 °C). The unit stops heating. At a higher demand level a room that is too warm only brings demand down, so heating goes off when even the lowest level is too much, not on a short overshoot in cold weather.

Heating starts again when the room falls below the target, after at least `idle.min` (30 minutes) off. It restarts at the lowest demand level.

### Cold weather

When it stays below `cold.enter` (−15 °C) outside for `cold.hold` (60 minutes), the controller runs the unit at demand 100% with quiet outdoor off. It then controls the room by moving the setpoint, 0.5 °C at a time, at most once every `cold.rate` (30 minutes). It starts at `cold.set` (23 °C).

It goes back to demand control when it stays above `cold.exit` (−10 °C) for 60 minutes.

### Defrosts

When the liquid temperature drops below `defrost.enter` (5 °C), the controller treats it as a defrost. It keeps its outputs as they are until the liquid temperature is back to `defrost.exit` (25 °C), or for at most `defrost.max` (30 minutes). The web status line shows "❄ defrost".

For `defrost.hold` (10 minutes) after a defrost, demand stays at `defrost.holdmax` or below and does not go up, so the unit settles after the defrost. The default 100% means 100% with quiet outdoor: plain 100% is never used in the hold, because without a limit the unit surges to full power after a defrost. Set 95% to hold it down further. In cold-weather mode the setpoint does not go up either. The integral keeps running, so it learns the heat the defrost cost, and demand can step up when the hold ends. The status line shows "after defrost". If a fan speed is set for the state, it is sent again, because the unit does not restore it after a defrost.

With `defrost.watchdog` on (it is off by default), if there are more than `defrost.loops` (3) defrosts within `defrost.window` (90 minutes), the controller turns heating off for `defrost.rest` (15 minutes). The status line shows "rest after defrost loop". This stops the unit from defrosting over and over.

### No room reading

A BLE reading counts as lost after 5 minutes without an update. An MQTT reading counts as lost after `t.control` (600 seconds).

While the reading is lost, the controller keeps its outputs as they are. After `auto.stale` (30 minutes) it hands over to the unit's own thermostat: setpoint = target, demand 100%, quiet outdoor off. When a reading comes back, it starts again in demand control.

### When it stops

When the controller stops for any reason (`auto.e` off, unit off, or the mode changes from heat), it sets demand to 100%, quiet outdoor off, and the setpoint to `stop.temp` (22 °C). If someone changed the setpoint at the same moment, that setpoint is kept.

### Changes from elsewhere

If the IR remote, Onecta or anything else changes the demand, setpoint, quiet outdoor or fan, the controller sets them back within a minute. Econo is kept off, because econo and demand use the same unit setting. To change these by hand, turn the controller off first.

The Home Assistant climate target and the MQTT `temp` command set `auto.t` while the controller is on and the unit is in heat mode. In other modes they set the unit's setpoint as before. The web *Target* always sets `auto.t`.

### After a restart

The controller keeps no state over a restart. It starts again in demand control at `demand.start` (60%).

## Settings

Set these on the *Advanced* settings page, or over MQTT. While the controller is on, the main settings page also has the target curve, the demand table and the fan settings. In MQTT JSON, leave out the dot: `auto.t` is `autot`, for example `setting/GuestAC {"pidkp":250}`. All settings in these tables take effect at once, except `t.control`: changing it restarts the device.

### Main

|Setting|Default|Meaning|
|-------|-------|-------|
|`auto.e`|off|Heating control on|
|`auto.t`|not set|Target room temperature, °C|
|`auto.curve`|empty|Target offset by outdoor temperature, see [Target curve](#target-curve)|
|`fan.flow`|see [Heat output and COP](#heat-output-and-cop)|Air flow by indoor fan speed, for the heat output estimate on the graph|
|`auto.b`|empty|BLE sensor to use for the room temperature|
|`auto.stale`|1800 s|No room reading for this long hands over to the unit's thermostat|
|`auto.smooth`|0 s|Room temperature smoothing. 0 for none. Try 300 s if your sensor reports in 0.1 °C steps and heat wanted jumps with each flicker|
|`stop.temp`|22 °C|Setpoint left on the unit when the controller stops|
|`t.control`|600 s|An MQTT room reading counts as lost after this long|

### Demand control

|Setting|Default|Meaning|
|-------|-------|-------|
|`demand.set`|30 °C|Setpoint sent to the unit. Many units cap the heat setpoint at 30 °C, so a higher value is resent every minute
|`demand.table`|see below|Demand levels and their watts|
|`demand.full`|on|Allow plain 100% (quiet outdoor off). Off: the highest level is 100% with quiet outdoor|
|`demand.start`|60%|Demand level to start at|
|`demand.rate`|600 s|Minimum time between demand changes|
|`demand.minrun`|30 min|Minimum heating time before heating off|
|`demand.fan`|Leave|Fan speed: `Leave`, `Auto`, `1`–`5` or `Quiet`|
|`pid.kp`|300 W/°C|Extra heat per degree below the target|
|`pid.ti`|90 min|Integral time. Shorter corrects a steady error faster. 0 turns the integral off: it stays where it is, so heat wanted is that plus `pid.kp` times the error (P only)|

### Heating off

|Setting|Default|Meaning|
|-------|-------|-------|
|`idle.temp`|16 °C|Setpoint while heating is off|
|`idle.over`|0.5 °C|Heating off when the room is this much above the target|
|`idle.under`|0 °C|Heating on again when the room is this much below the target|
|`idle.min`|30 min|Minimum time heating off|
|`idle.fan`|Leave|Fan speed while heating is off|

### Cold weather

|Setting|Default|Meaning|
|-------|-------|-------|
|`cold.enter`|−15 °C|Cold-weather mode below this outdoor temperature|
|`cold.exit`|−10 °C|Back to demand control above this outdoor temperature|
|`cold.hold`|60 min|The outdoor temperature must stay past `cold.enter` or `cold.exit` this long|
|`cold.set`|23 °C|Starting setpoint|
|`cold.rate`|1800 s|Minimum time between 0.5 °C setpoint steps|
|`cold.kp`|2.0|Setpoint change, °C per degree below the target|
|`cold.ti`|120 min|Integral time. 0 turns the integral off, so the setpoint is `cold.set` plus `cold.kp` times the error|
|`cold.fan`|Leave|Fan speed in cold-weather mode|

The cold-weather gains have not been tested on a real unit yet.

### Defrost

|Setting|Default|Meaning|
|-------|-------|-------|
|`defrost.enter`|5 °C|A defrost starts when the liquid temperature drops below this|
|`defrost.exit`|25 °C|The defrost is over when the liquid temperature is back to this|
|`defrost.max`|30 min|The defrost is over after this long anyway|
|`defrost.hold`|10 min|After a defrost, demand does not go up for this long. 0 for no hold|
|`defrost.holdmax`|100%|Highest demand in the hold. 100 is 100% with quiet outdoor; plain 100% is never used in the hold|
|`defrost.watchdog`|off|Turn heating off for a while after a defrost loop (more defrosts than `defrost.loops` within `defrost.window`)|
|`defrost.loops`|3|More defrosts than this…|
|`defrost.window`|90 min|…within this long…|
|`defrost.rest`|15 min|…turn heating off for this long|

A liquid temperature drop counts as a defrost only after the coil has been hot (at `defrost.exit`) since heating started. So a coil that cooled down while heating was off is not taken for a defrost.

### Demand table

`demand.table` lists the demand levels the controller may use, as `demand:watts`, lowest first. `100q` is 100% with quiet outdoor. The default is:

```
40:270,50:340,60:400,70:465,75:500,80:530,85:570,90:605,95:640,100q:1510,100:2470
```

The watts are the power one unit drew at each level. Up to 95% they follow about 6.7 W per %: 50, 60 and 70% were measured at −1…−2 °C outside, and the other levels lie on the same line. The 75% and 90% levels make the steps more even, about 30–65 W each. The unit draws more at the same level in colder weather. The controller picks the highest level whose watts are at or below the heat it wants. The loop corrects for errors in the table, so the order of the levels matters more than the exact watts. An invalid table is reported on `error/GuestAC/heatctl` and the default is used instead.

### Target curve

`auto.curve` raises or lowers the target with the outdoor temperature. It is a list of `outdoor:offset` points, for example `-25:1.0,-10:0.5,0:0`: 1 °C warmer at −25 °C outside, 0.5 °C at −10 °C, and no change at 0 °C and above. Between points the offset is interpolated linearly. Beyond the ends it stays at the end value. The outdoor temperature is smoothed over 30 minutes.

The web status line shows the curve offset to 0.01 °C, for example "target 21.3 (curve +0.30)". It is left out when the offset is 0. An invalid curve is reported on `error/GuestAC/heatctl` and no offset is used.

## Tuning

Start with the defaults and watch the room for a day or two.

- The room stays below the target for hours: raise `pid.kp`, or shorten `pid.ti`.
- The room swings slowly above and below the target: lower `pid.kp`, or lengthen `pid.ti`.
- Heating stops and starts too often: raise `idle.over` or `idle.under`, for example to 0.8 and 0.3.

Known problem: in a room that heats up and cools down slowly, where the heat needed is just below the `100q` level, the controller can swing between demand control and heating off over about 12 hours.

## Room temperature over MQTT

Without a BLE sensor, send the room temperature in a `control` command at least every `t.control` seconds:

```
command/GuestAC/control {"env":21.3}
```

Or set `auto.topic` to a topic to follow and `auto.payload` to the JSON field in its payload that holds the temperature.

## Watching it

- **Status:** the status JSON (`state/GuestAC/status`) has `ctl` (state), `ctl_sub` (`defrost`, `hold` or `rest`), `ctl_target` (target with the curve offset), `ctl_curve` (the curve offset), `ctl_heatw` (heat wanted, W) and `ctl_i` (the integral). `ctl_i` is in watts, except in cold-weather mode, where it is the setpoint integral in °C.
- **MQTT:** each change of state is sent to `info/GuestAC/heatctl`, with the reason and the inputs.
- **Home Assistant:** a "Control state" sensor, "Heat wanted", "Heating integral" and "Effective target" sensors, and numbers for "Heating target", "Heating gain" (`pid.kp`) and "Heating integral time" (`pid.ti`). `hvac_action` shows heating, idle or defrosting.
- **Graph:** the *Heating graph* link at the bottom of the main page, above *Settings*, opens `/graph`. It shows four charts for up to the last 3 days. The first has the room temperature as the sensor reads it and as the controller sees it after `auto.smooth`, the target and the effective target, plus the setpoint, the setpoint integral at full power and the outdoor temperature, which are hidden until you click them in the legend. The second has heat wanted, its integral and proportional parts, power (the unit's power consumption) and demand. The proportional part, `pid.kp` times the error, is small next to the others, so it has its own axis on the right. The third has the unit's liquid temperature, the unit's own room reading, the compressor frequency (Hz) and the indoor fan speed (rpm) as the unit reports it, which show how the unit responds to each demand level and what a defrost looks like. The fan speed is the real one, so it shows when the unit slows the fan because the coil is cool, whatever fan setting is chosen. The fourth has the estimated heat output next to the power consumption, and the COP (heat output divided by power): each sample's, and the mean over the last hour, which counts the defrosts in. See [Heat output and COP](#heat-output-and-cop) for how far to trust them. Heating off, cold weather, no room reading, defrosts, the hold after a defrost and the rest after a defrost loop are shaded. A dashed line marks each change of kp or ti (`pid.kp`/`pid.ti`, or `cold.kp`/`cold.ti` at full power), and the tooltip shows the values in use, with the fan setting and whether quiet outdoor is on. The device keeps one sample every 15 seconds, 3 days of them (about 760 KB of PSRAM), or 1.5 hours on boards without PSRAM. The samples are lost when the device restarts. Reload the page to update it, or choose an interval under *Refresh* and the page reloads itself; it is off unless you choose one. A refresh keeps the range chosen with the buttons below, but not a zoom made by dragging. Another interval can be given in the address, for example `/graph?refresh=120` for two minutes. With a mouse, drag across a chart to zoom in on that time range, or ctrl+scroll; shift+drag pans. On a phone or tablet, pinch to zoom and drag to pan. The four charts move together. The *All* button shows the whole log, and *1 day*, *12 h* and *3 h* show the latest day, 12 hours or 3 hours. A range button is hidden while the log is shorter than it. Heat wanted and power are both electrical watts, so you can check the power the unit draws at each demand level against `demand.table`. The samples are also available as JSON at `/graph.json`.
- **Log:** each change of state with its reason, for example `heatctl: DEMAND -> IDLE (…)`, and a status line every 10 minutes.

`no.faikoutauto` (*Hide heating control*) hides the heating control from Home Assistant. It also hides it from the web page, unless the controller is on and no settings password is set.

## Heat output and COP

The unit does not measure the heat it gives out, so Säädin estimates it from the air that passes through the indoor unit:

heat output (W) = 0.335 × air flow (m³/h) × (liquid temperature − unit's room reading)

- The air flow comes from the indoor fan speed the unit reports, through `fan.flow`.
- The unit's own room reading is the temperature of the air it takes in.
- The liquid temperature is close to the temperature of the air it blows out (measured on Perfera units by forum users).

COP is the heat output divided by the power consumption the unit reports.

`fan.flow` is a list of up to 8 `rpm:m³/h` points, linear between points and flat beyond the ends. The estimate uses the fan speed the unit reports, not the fan setting, so a fan the unit has slowed for a cool coil, or any speed between two settings, gets the air flow between the nearest points. Set `fan.flow` blank for no estimate.

The default, `0:0,510:342,570:426,660:534,750:636,940:828,1120:1104,1400:1380`, is for a Perfera 40:

|Fan setting|Fan speed|Air flow|
|-------|-------|-------|
|Quiet|510 rpm|342 m³/h|
|1|570 rpm|426 m³/h|
|2|660 rpm|534 m³/h|
|3|750 rpm|636 m³/h|
|4|940 rpm|828 m³/h|
|5|1120 rpm|1104 m³/h|

The fan speeds were read from one unit at each setting. The air flows are from a forum post, taken here as quiet and fan 1 to 5. Below quiet the air flow falls in a line to nothing at a stopped fan, and the last point, 1400 rpm, carries the line on above fan 5.

Other models move a different amount of air at the same fan speed. To set it for your unit, set each fan speed in turn while heating, read the fan speed (rpm) at each from the graph, where the tooltip shows the fan setting, and pair it with the air flow from the data sheet. Daikin's tables give 306, 360, 600 and 1020 m³/h in heating for silent, low, medium and high on a Perfera 40 (FTXTM40A and S), and 282, 336, 462 and 702 m³/h on a Perfera 30 (FTXTM30A).

Treat the numbers as rough. The air flow is not measured, the temperatures come in 0.5 °C steps, and the liquid temperature is not exactly the outlet air temperature. A change in COP from one demand level to another on the same unit means more than the value itself. During a defrost the fan stops, so the heat the defrost takes from the room is not counted; the 1 h mean still counts the power the defrost used.
