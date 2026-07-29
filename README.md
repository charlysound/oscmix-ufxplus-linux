# oscmix UFX+ GTK4 for Linux

An unofficial GTK4 control application for the **RME Fireface UFX+** connected
over USB in **Class Compliant mode**.

This project has its own compact three-row mixer interface. It is inspired by
hardware mixer workflows, but it is not a clone of TotalMix FX and does not use
the same GUI.

[![GTK4 checks](https://github.com/charlysound/oscmix-ufxplus-linux/actions/workflows/gtk4-check.yml/badge.svg?branch=main)](https://github.com/charlysound/oscmix-ufxplus-linux/actions/workflows/gtk4-check.yml)

> [!WARNING]
> This is pre-release remote-control software for live audio hardware. Test it
> with monitor levels reduced and keep a hardware mute available.

> [!NOTE]
> This project is not sponsored, authorized or endorsed by RME. RME, Fireface
> and TotalMix are trademarks of their respective owners.

## What this application provides

- three horizontally scrollable mixer rows: Inputs, Playback and Outputs;
- submix routing with faders, Pan/Balance, Mute and Solo where applicable;
- stereo pairing with one strip, one fader and separate left/right meters;
- UFX+ input controls such as gain, phantom power and Hi-Z when the selected
  channel exposes them;
- channel meters with Dynamics gain-reduction indication;
- input and hardware-output EQ and Dynamics editors;
- compact inline DSP editors, a collapsible inspector and detachable windows;
- editable channel EQ and Room EQ graphs;
- Room EQ for hardware outputs, including optional stereo linking;
- automatic discovery of the UFX+ ALSA sequencer port;
- reconnection after USB disconnects or ALSA port renumbering.

Playback strips intentionally do not show EQ or Dynamics controls because those
controls are not exposed for playback channels by the current UFX+ mapping.

## How control reaches the interface

The UFX+ does not receive OSC directly. The packaged launcher starts three
local components:

```text
GTK4 interface ⇄ local OSC/UDP ⇄ oscmix engine ⇄ ALSA MIDI SysEx ⇄ UFX+
```

OSC is an internal transport between the GUI and the engine. The connection to
the hardware uses the UFX+ Class Compliant MIDI control port.

## Requirements and scope

| Item | Current status |
|---|---|
| RME Fireface UFX+ over USB | Primary target |
| UFX+ Class Compliant mode | Required |
| Linux with ALSA | Required |
| x86-64 | Debian package and AppImage |
| ARM64 | AppImage |
| Thunderbolt control on Linux | Not supported |
| Other RME interfaces | Not validated by this GTK4 application |
| TotalMix FX feature parity | Not claimed |

The application models all UFX+ input, playback and output names used by the
control protocol. Which audio channels are available to a DAW still depends on
the interface firmware, Class Compliant mode and sample-rate configuration.

## Install a release

The current pre-release is
[v0.1.0-alpha.3](https://github.com/charlysound/oscmix-ufxplus-linux/releases/tag/v0.1.0-alpha.3).

### Debian package

Download the `.deb` and `SHA256SUMS`, then run:

```shell
sha256sum --check SHA256SUMS
sudo apt install ./oscmix-ufxplus_0.1.0.alpha3-1_amd64.deb
```

The downloadable filename uses a dot because GitHub normalizes Debian's `~`
character. The installed package version is correctly recorded as
`0.1.0~alpha3-1`.

### AppImage

Download the AppImage and matching `.sha256` file for your architecture.
For x86-64:

```shell
sha256sum --check \
  oscmix-ufxplus-gtk4-0.1.0-alpha.3-x86_64.AppImage.sha256
chmod +x oscmix-ufxplus-gtk4-0.1.0-alpha.3-x86_64.AppImage
./oscmix-ufxplus-gtk4-0.1.0-alpha.3-x86_64.AppImage
```

Use the `aarch64` files on ARM64. The AppImages bundle the GTK4 runtime, but
device discovery uses the host's `aconnect` command:

```shell
sudo apt install alsa-utils iproute2
```

## Build from source

On Debian, Ubuntu or a derived distribution:

```shell
sudo apt update
sudo apt install build-essential git pkg-config libasound2-dev \
  libavahi-client-dev libgtk-4-dev

git clone https://github.com/charlysound/oscmix-ufxplus-linux.git
cd oscmix-ufxplus-linux
make -j2 oscmix alsaseqio gtk4
make -C gtk4 check
make -C gtk4 install-local
```

`install-local` installs the application under the current user's
`~/Applications` directory and adds a desktop-menu entry. It does not install
system-wide files.

## Hardware setup

1. Put the UFX+ in Class Compliant mode.
2. Connect it over USB, not Thunderbolt.
3. Confirm that ALSA sees its MIDI ports:

   ```shell
   aconnect -l
   ```

4. Start **oscmix GTK4 – UFX+** from the application menu, or run the downloaded
   AppImage.

The launcher selects the control port by the UFX+ device name instead of saving
a temporary ALSA client number. To inspect discovery without opening the GUI:

```shell
./gtk4/AppRun --list-devices
```

## Important routing limitation

The UFX+ Class Compliant state refresh used by this project does not return the
complete mixer routing matrix. The GTK4 application therefore stores the
input and playback sends that it controls in the current user's XDG state
directory and restores its saved display state on the next launch.

Before a saved state exists, the GUI displays a direct 24-channel
Playback-to-Output layout. Loading this display state does not itself write
routes to the interface.

## Troubleshooting

### The window stays on “Connecting to oscmix”

- start the packaged launcher or AppImage, not only the GUI binary;
- confirm that `aconnect -l` lists a Fireface UFX+;
- confirm that the interface is connected by USB in Class Compliant mode;
- inspect the launcher log under
  `${XDG_RUNTIME_DIR:-/tmp}/oscmix-gtk4-$UID-*.log`.

### The interface disappeared after reconnecting USB

Leave the GTK4 window open. The launcher waits for a stable ALSA enumeration
and restarts the local MIDI/OSC backend when the UFX+ returns.

### A displayed route does not match the audible route

The GUI cannot read the complete hardware routing matrix in the current UFX+
Class Compliant implementation. Verify the saved GTK4 routing state and test
with monitoring levels reduced.

## Project status

This application is under active development and is validated primarily with a
Fireface UFX+ on Linux. A control being visible does not guarantee validation
on every firmware version or sample-rate configuration.

When reporting a problem, include:

- Linux distribution and kernel;
- UFX+ firmware version;
- USB Class Compliant mode and sample rate;
- whether the `.deb`, AppImage or source build was used;
- exact reproduction steps.

Use [GitHub Issues](https://github.com/charlysound/oscmix-ufxplus-linux/issues)
for bugs and feature requests. Report security problems through the private
process described in [SECURITY.md](SECURITY.md).

## Development

The GTK4-specific implementation is in [`gtk4/`](gtk4). See
[`gtk4/README.md`](gtk4/README.md) for its architecture and developer commands.
Contribution requirements are documented in
[`CONTRIBUTING.md`](CONTRIBUTING.md).

User-facing application text, source comments and GitHub documentation are
maintained in English.

## Attribution and license

This work retains the OSC/MIDI engine and protocol foundations from
[huddx01/oscmix](https://github.com/huddx01/oscmix), derived from
[michaelforney/oscmix](https://github.com/michaelforney/oscmix).

See [LICENSE](LICENSE) and [debian/copyright](debian/copyright) for the complete
copyright notices and license terms.
