# Advanced operation

There are a number of settings and commands that are available via MQTT. To use these you must have set up the device to connect to an MQTT server. You then need a means to send messages to the MQTT server for the device.

The *hostname* you picked is used for these messages. The format for commands and settings is defined in [RevK library](https://github.com/revk/ESP32-RevK). But is basically quite simple. We'll use `GuestAC` as the example hostname.

|Type|Format|Meaning|
|----|------|-------|
|Command|`command/GuestAC/command`|This issues commands to the device to do something now.|
|Setting|`setting/GuestAC`|This allows settings to be set. The payload is JSON, with one or more settings. e.g. `setting/GuestAC {"fanstep":2,"reporting":60}`. With no payload the existing settings are returned in the same format.|
|Setting (direct)|`setting/GuestAC/thing`|This allows a single setting to be set, the payload is the value, e.g. `setting/GuestAC/debug 1`|
|State|`state/GuestAC/whatever`|Status messages - these are normally retained and relate to specific aspects of the operation. With no paramater, e.g. `state/GuestAC` this is the state of the device itself, with `false` meaning it is now offline.|
|Event|`event/GuestAC/whatever`|Event messages are one of things that have happened and are not retained.|
|Info|`info/GuestAC/whatever`|Info messages are informational, and don't relate to a specific event happening.|
|Error|`error/GuestAC/whatever`|Errors are reported using this.|

## Heating control

Säädin replaces *Faikout auto* with its own heating controller. It acts only in heat mode, never turns the aircon on or off, and leaves cool, dry and fan mode alone. [docs/heat-control.md](../docs/heat-control.md) describes how it works and its settings (`auto.e`, `auto.t`, `auto.curve`, `demand.*`, `pid.*`, `idle.*`, `cold.*`, `defrost.*`).

## Settings

There are a number of standard settings as per the [RevK library](https://github.com/revk/ESP32-RevK), including things like `hostname`, `mqtthost`, `wifissid`, etc. Settings specific to the Faikout module are as follows.

The web pages provide settings for the basic WiFi/MQTT, the Faikout specific settings, and an Advanced settings page listing a lot of more detailed settings.

Note that web settings can be disabled with `websettings`, and the web based control pages can be disabled with `webcontrol`. It is also possible to apply a password for the web settings 9this is not sent security, so use with care on a local network which you control).

### Room temperature

The heating control needs the room temperature, from a BLE sensor (`autob`) or over MQTT. Send `env` in a `control` command at least every `tcontrol` seconds, e.g. `command/GuestAC/control {"env":21.3}`.

### Reference temperature

Instead of `env` in `control`, you can set `autotopic` to a topic to subscribe to, and `autopayload` to the field name in its JSON payload. A message on that topic sets the room temperature.

### Special settings

Some more advances settings which you are unlikely to need to ever change.

|Setting|Meaning|
|-------|-------|
|`debug`|`true` means output lots of debug - notable for S21 this is one line with a set of poll responses. This also causes more fields to be polled than normal, so slower response times.|
|`dump`|`true` means output raw serial communications|
|`uart`|Which internal UART to use|
|`tx`|Which GPIO for tx, prefix `-` to invert the port|
|`rx`|Which GPIO for rx, prefix `-` to invert the port|

## Commands

|Command|Meaning|
|-------|-------|
|`on` `off`|Power on/off|
|`heat` `cool` `auto` `fan` `dry`|Change mode|
|`low` `medium` `high`|Change fan speed|
|`temp`|Set target temp (argument is temp)|
|`status`|Force a status report to be sent|
|`control`|JSON payload with aircon controls, see below|
|`send`|Force sending message for debug / testing|

The `send` command when used for S21 is normally a string, or an array of strings, and each string is the payload to send, e.g. `D62000`. Use `\u0080` to `\u00FF` for high bytes.

The `send` command when used for X50A is an object with `"cmd"` the command (two byte hex string), and `"data"` (string of hex bytes).

## Status

Regular status messages are sent.

- `state/` topic indicate current state, and are reported periodically and on some state changes.
- `Faikout/` topic are sent, typically every minute, and intended for the `faikoutlog` command to store in a database.
- `*MAC*/` topic are sent for HomeAssistant if enabled, and are reported periodically and on some state changes.

The setting `livestatus` causes the `state/` topic on any change.

|Attribute|Meaning|
|---------|-------|
|`online`|Boolean, if the aircon is connected and online|
|`heat`|Boolean, if in heating mode|
|`slave`|Boolean, set if we are not master for heat/cool and hence cannot do requested mode|
|`antifreeze`|Boolean, set if in antifreeze mode and hence not operating as normal|
|`model`|Model name, if known|
|`home`|Temperature at remote / measured|
|`outside`|Outside temperature, if known|
|`inlet`|Inlet temperature, if known|
|`liquid`|Liquid coolant feed temperature, if known|
|`control`|Boolean, if we are under external/automatic control|

The `faikoutlog` reports the last periods for values. For each value, if it is the same for the whole period it is reported as is. If not, then for numeric is reported as an array of *min*, *ave*, *max*. For an enumerated type it is the current value. For a Boolean, it is a value `0.0` to `1.0` indicating how much it was `true` in the period.

The `fixstatus` setting forces the format as if the value had changed during the period, i.e. min/ave/max array or 0.0-1.0 for Boolean.

## Aircon control

The controls are things you can change. These can be sent in a JSON payload in an MQTT `control` command (with no suffix), and are reported in the `status` MQTT JSON.

|Attribute|Meaning|
|---------|-------|
|`power`|Boolean, if aircon powered on|
|`mode`|One of `H` (Heat), `C` (Cool), `A` (Auto), `D` (Dry), `F` (Fan)|
|`temp`|Temperature, in Celcius|
|`fan`|One of `A` (Auto), `Q` (Night) and `1` to `5` for manual fan levels|
|`swingh`|Boolean - horizontal louvre swing|
|`swingv`|Boolean - vertical louvre swing|
|`powerful`|Boolean|
|`econo`|Boolean|
|`env`|Room temperature for the heating control|
|`streamer`| Boolean|

There are also settings for the heating control. These are also reported in the status messages.

|Attribute|Meaning|
|---------|-------|
|`autoe`|Heating control on. This sets the `autoe` setting|
|`autot`|Target room temperature. This sets the `autot` setting|
|`autob`|The name of the BLE device. This sets the `autob` setting|

## Debug

All of the protocols are reverese engineered, and some times now things come to light and new models.

The `snoop` and `dump` and `debug` settings can help decode what is happening.

For anyone in the UK trying to reverse engineer operations using an offical remote / control, we have a small number of dual port *pass through* modules to assist with debug.

<img src='https://github.com/revk/ESP32-Faikout/assets/996983/5f998a5f-d99d-40ca-bf39-fd1206c664df' width=50%><img src='https://github.com/revk/ESP32-Faikout/assets/996983/6c45b348-035e-48a7-81fb-dc43c849b11e' width=50%>
