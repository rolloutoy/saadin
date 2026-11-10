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

When the room is more than `idle.over` (0.5 °C) above the target, and heating has run for at least `demand.minrun` (30 minutes), the controller sets the setpoint to `idle.temp` (16 °C). The unit stops heating.

Heating starts again when the room falls below the target, after at least `idle.min` (30 minutes) off. It restarts at the lowest demand level.

### Cold weather

When it stays below `cold.enter` (−15 °C) outside for `cold.hold` (15 minutes), the controller runs the unit at demand 100% with quiet outdoor off. It then controls the room by moving the setpoint, 0.5 °C at a time, at most once every `cold.rate` (10 minutes). It starts at `cold.set` (23 °C).

It goes back to demand control when it stays above `cold.exit` (−10 °C) for 15 minutes.

### Defrosts

When the liquid temperature drops below `defrost.enter` (5 °C), the controller treats it as a defrost. It keeps its outputs as they are until the liquid temperature is back to `defrost.exit` (25 °C), or for at most `defrost.max` (30 minutes). The web status line shows "❄ defrost".

For `defrost.hold` (10 minutes) after a defrost, demand stays at 95% or below. The status line shows "after defrost". If a fan speed is set for the state, it is sent again, because the unit does not restore it after a defrost.

If there are more than `defrost.loops` (3) defrosts within `defrost.window` (90 minutes), the controller turns heating off for `defrost.rest` (15 minutes). The status line shows "rest after defrost loop". This stops the unit from defrosting over and over.

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
|`auto.b`|empty|BLE sensor to use for the room temperature|
|`auto.stale`|1800 s|No room reading for this long hands over to the unit's thermostat|
|`auto.smooth`|300 s|Room temperature smoothing. 0 for none|
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
|`pid.ti`|90 min|Integral time. Shorter corrects a steady error faster|

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
|`cold.hold`|15 min|The outdoor temperature must stay past `cold.enter` or `cold.exit` this long|
|`cold.set`|23 °C|Starting setpoint|
|`cold.rate`|600 s|Minimum time between 0.5 °C setpoint steps|
|`cold.kp`|2.0|Setpoint change, °C per degree below the target|
|`cold.ti`|120 min|Integral time|
|`cold.fan`|Leave|Fan speed in cold-weather mode|

The cold-weather gains have not been tested on a real unit yet.

### Defrost

|Setting|Default|Meaning|
|-------|-------|-------|
|`defrost.enter`|5 °C|A defrost starts when the liquid temperature drops below this|
|`defrost.exit`|25 °C|The defrost is over when the liquid temperature is back to this|
|`defrost.max`|30 min|The defrost is over after this long anyway|
|`defrost.hold`|10 min|After a defrost, demand 95% or below for this long. 0 for no hold|
|`defrost.loops`|3|More defrosts than this…|
|`defrost.window`|90 min|…within this long…|
|`defrost.rest`|15 min|…turn heating off for this long|

A liquid temperature drop counts as a defrost only after the coil has been hot (at `defrost.exit`) since heating started. So a coil that cooled down while heating was off is not taken for a defrost.

### Demand table

`demand.table` lists the demand levels the controller may use, as `demand:watts`, lowest first. `100q` is 100% with quiet outdoor. The default is:

```
40:220,50:280,60:350,70:400,80:520,85:580,95:640,100q:1510,100:2470
```

The watts are the power the unit drew at each level in a test on one unit. The controller picks the highest level whose watts are at or below the heat it wants. The loop corrects for errors in the table, so the order of the levels matters more than the exact watts. An invalid table is reported on `error/GuestAC/heatctl` and the default is used instead.

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
- **Log:** each change of state with its reason, for example `heatctl: DEMAND -> IDLE (…)`, and a status line every 10 minutes.

`no.faikoutauto` (*Hide heating control*) hides the heating control from Home Assistant. It also hides it from the web page, unless the controller is on and no settings password is set.
