# Säädin

Local ESP32 firmware for Daikin heat pumps and air conditioners. MQTT, Home Assistant and BLE room sensors, no cloud.

Säädin is a fork of [Faikout](https://codeberg.org/RevK/ESP32-Faikout) by Adrian Kennard ([RevK](https://codeberg.org/RevK)). The aim is to run the heat pump at a steady, modulated output: the firmware sets the unit's power limit ("demand") from a room temperature control loop instead of switching it on and off around a setpoint.

> **Status: early fork.** Apart from the [changes listed below](#changes-from-faikout), the code is still upstream Faikout. The app is named `Saadin`, but most source files and settings still use Faikout names. There are no Säädin releases yet.

## Credit

Almost all of this code is Faikout, written by Adrian Kennard and sponsored by [Andrews & Arnold Ltd](https://www.aa.net.uk/), with contributions from others. That includes:

- the reverse-engineered Daikin S21 and X50A protocols
- the web interface
- MQTT and Home Assistant integration
- BLE sensor support
- the ESP32 support library

Säädin only builds on that work.

The original project:

- Firmware and documentation: [codeberg.org/RevK/ESP32-Faikout](https://codeberg.org/RevK/ESP32-Faikout)
- Hardware: Faikout boards from [shop.revk.uk](https://shop.revk.uk/). Buying them supports the original author.
- Bundled libraries, all by RevK and kept with their own licences:
  - [ESP32-RevK](https://codeberg.org/RevK/ESP32-RevK) (`ESP/components/ESP32-RevK`)
  - [ESP32-BLE-Env](https://codeberg.org/RevK/ESP32-BLE-Env)
  - [AJL](https://codeberg.org/RevK/AJL)
  - [AXL](https://codeberg.org/RevK/AXL)
  - [SQLlib](https://codeberg.org/RevK/SQLlib)
  - [stringdecimal](https://codeberg.org/RevK/stringdecimal)

The commit history from the original project is kept, so each upstream author's work is still attributed to them.

**Please do not report Säädin problems to the Faikout project.** If you can reproduce a problem with unmodified Faikout firmware, report it upstream; otherwise open an issue here.

## Not affiliated

Säädin is an independent fork. It is not affiliated with, endorsed by or supported by Andrews & Arnold Ltd, Adrian Kennard or Daikin.

- *Faikout* and the A&A and AJK logos are trademarks of Andrews & Arnold Ltd.
- *Daikin* is a trademark of Daikin Industries, Ltd.

These names appear here only to describe where the code comes from and what it works with.

## Hardware

Säädin is firmware only. It is meant to run on [Faikout boards](https://shop.revk.uk/), which plug into the Daikin's S21 connector (or X50A on larger and ducted units). This repository has no hardware designs. The KiCad PCB designs, case models and production files are in the [upstream repository](https://codeberg.org/RevK/ESP32-Faikout).

**Safety:** always isolate the power before opening an air conditioner. Some connector types carry dangerous voltages.

## Features (inherited from Faikout)

- Local web control with live status over a websocket. No app and no internet connection needed.
- MQTT reporting and control.
- Home Assistant auto-discovery over MQTT.
- Detects automatically whether the unit uses the S21 or X50A protocol.
- BLE temperature sensors (BlueCoinT, Telink/Xiaomi) can be used as the room reference for automatic control.
- Linux MySQL/MariaDB logging and graphing tools (`Tools/`).

## Planned

- **Heating control, next steps:** slow on/off cycling below the lowest demand, time-proportioning in the gap between 95% and 100% with quiet outdoor, and optional outdoor-temperature feedforward.
- **Releases:** builds published as GitHub Releases, with Säädin naming throughout.

## Building

Build and flash from source with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/). [mise](https://mise.jdx.dev/) sets up the tools:

```sh
mise install          # eim (ESP-IDF installer), prek, and popt for the host tools
eim install           # once: install ESP-IDF; `eim select` picks the version
```

In this directory mise then puts the selected ESP-IDF on your PATH, so no `source …/activate_idf_*.sh` is needed. Without mise, activate ESP-IDF yourself and install popt (e.g. `brew install popt`).

```sh
cd ESP
make pico     # original Faikout board (ESP32-PICO)
make s3       # ESP32-S3 boards
make s3eth    # ESP32-S3 Ethernet board
make flash
```

`ESP/Makefile` has targets for the other chips.

Libraries are included as git subtrees, so a plain clone is enough; `--recursive` is not needed. Boards can be flashed through the USB test pads, or with a [TC2030-USB-NL](https://www.tag-connect.com/product/tc2030-usb-nl) lead on boards with a Tag-Connect footprint.

> **No OTA downloads, no signing.** Säädin never fetches firmware from a server. There is no OTA host, no automatic update check, and no `upgrade` command or button. Builds are not signed. To update, download a release and upload the `.bin` on the "Firmware" settings tab.
>
> A device running upstream Faikout rejects Säädin images over OTA, because they are unsigned and have a different app name. Flash the first Säädin build over USB. To go back, upload an upstream Faikout `.bin` on the "Firmware" tab; Säädin accepts it.

## Set-up and documentation

When first started, the device acts as a Wi-Fi access point with a set-up page. After it joins your network, open `http://<hostname>.local/` for the control page.

These manuals come from upstream and still use the Faikout name:

- [Setup](Manuals/Setup.md)
- [Controls](Manuals/Controls.md)
- [Heating control](Manuals/Heating.md) (new in Säädin)
- [Advanced](Manuals/Advanced.md)
- [BLE sensors](Manuals/BLE.md)
- [S21 protocol](Manuals/S21.md). The [upstream wiki](https://codeberg.org/RevK/ESP32-Faikout/wiki/S21-Protocol) is more up to date.

## Changes from Faikout

Säädin is a modified version of Faikout. In short, it differs like this. [CHANGELOG.md](CHANGELOG.md) has every change in detail, with dates, as required by GPL-3.0 §5(a).

- **Heating control.** A new heating controller replaces Faikout auto. It holds the room at a target by limiting demand, turns heating off when the room is warm, controls by setpoint in very cold weather, holds its outputs through defrosts, and can set the fan by outdoor temperature. See [Manuals/Heating.md](Manuals/Heating.md) and [docs/heat-control.md](docs/heat-control.md).
- **Heating graph.** A new `/graph` page charts up to 5 days of temperatures, demand, power, estimated heat output and COP, to help tune the controller. The samples are also at `/graph.json`.
- **Removed from Faikout auto.** Timed on/off, auto power on/off by temperature, heat/cool switching, auto fan and their settings are gone.
- **BLE.** Only room temperature sensors are supported. The Faikout BLE remote is ignored.
- **Firmware updates.** Firmware is uploaded from the browser and starts on probation: it rolls back to the previous firmware unless it is kept. Upstream Faikout images are accepted too. OTA downloads from a server are removed, and builds are not signed.
- **Home Assistant and MQTT.** New sensors and number entities for the heating controller, and its state is in the status JSON and sent on every change.
- **Name.** The app is `Saadin` instead of `Faikout`: binaries, setup access point, Home Assistant device model and app-wide MQTT topics. Settings are kept.
- **Defaults.** The timezone is Finnish time, and `auto.e` is off.
- **Fixes.** Several memory leaks, crashes and web server lock-ups inherited from Faikout are fixed.
- **Repository.** PCB designs, case models, product photos, release binaries and the upstream release tooling are removed; they are still available upstream. All C code is reformatted to ESP-IDF style.

## Security

Report vulnerabilities privately through GitHub's [private vulnerability reporting](https://github.com/rolloutoy/saadin/security/advisories/new), not in public issues.

## Licence

GPL-3.0, the same licence as Faikout. See [LICENSE](LICENSE).

- Copyright in the upstream code belongs to Adrian Kennard, Andrews & Arnold Ltd and the other Faikout contributors.
- Changes made in Säädin are released under the same licence.
