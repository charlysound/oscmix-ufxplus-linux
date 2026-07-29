# GTK4 frontend for the Fireface UFX+

This directory contains the UFX+-specific GTK4 interface. It is a custom mixer
GUI, not a port or visual copy of TotalMix FX.

## Components

- `main.c` creates the application window, header and inspector;
- `mixer_row.c` renders the virtualized Inputs, Playback and Outputs rows;
- `dsp_panel.c` implements channel EQ, Dynamics and Room EQ editors;
- `model.c` stores mixer state and translates OSC events into UI state;
- `transport.c` runs the local OSC transport outside the GTK main thread;
- `AppRun` discovers the UFX+ ALSA port and supervises the backend processes.

The mixer matrix is stored in compact arrays. Channel strips are drawn by
custom GTK widgets, and only the visible horizontal region is rendered.

## Build and test

From the repository root:

```shell
sudo apt install build-essential pkg-config libgtk-4-dev \
  libasound2-dev libavahi-client-dev
make -j2 oscmix alsaseqio gtk4
make -C gtk4 check
```

The test target covers the UFX+ model, channel naming, route persistence and
OSC bundle transport.

## Install for the current user

```shell
make -C gtk4 install-local
```

This installs the launcher and binaries below `~/Applications/oscmix` and adds
a desktop-menu entry without requiring root privileges.

## Run the GUI separately

For UI development against an already running oscmix engine:

```shell
./gtk4/oscmix-gtk4 --send-port 7222 --recv-port 8222
```

Normal users should start the installed launcher or AppImage. The launcher
chooses free local OSC ports, discovers the UFX+ by ALSA device name and
restarts the backend after USB or ALSA re-enumeration.

List detected UFX+ control ports without opening the GUI:

```shell
./gtk4/AppRun --list-devices
```

## Rendering

GTK selects the available GSK renderer. OpenGL or Vulkan may be used when
available, with GTK's software renderer as the fallback. Meter animation is
suspended while the window is minimized.

## Routing state

The current UFX+ Class Compliant refresh does not expose the complete hardware
routing matrix. The frontend persists only the input and playback sends that
it controls. This cached display state is not a substitute for reading the
hardware matrix.

## Scope

The GTK4 frontend is validated for the Fireface UFX+ over USB in Class
Compliant mode. Other RME devices and Thunderbolt control are outside its
current supported scope.
