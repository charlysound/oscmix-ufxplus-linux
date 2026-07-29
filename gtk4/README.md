# oscmix GTK4 — UFX+ prototype

This directory contains the lightweight GTK 4 frontend. It deliberately runs
alongside the existing GTK 3 application while feature parity is being built.

The routing matrix is stored as compact numeric arrays rather than one GTK
object per route. The three mixer rows are custom GTK snapshot widgets that
only render visible channel strips.

## Build

```sh
sudo apt install libgtk-4-dev
make gtk4
```

## Run against an existing oscmix engine

```sh
./gtk4/oscmix-gtk4 --send-port 7222 --recv-port 8222
```

The installed `AppRun` launcher does not retain ALSA's temporary numeric port.
It tracks the selected UFX+ by its stable ALSA device name, waits for a stable
enumeration and restarts the MIDI/OSC backend after a USB disconnect or port
renumbering. The window stays open while offline. A lightweight heartbeat and
backend-session identifier make the frontend request a complete refresh after
reconnection. Retry backoff is generic and contains no USB-controller, PCI-path
or host-specific rule.

The prototype currently targets the RME Fireface UFX+ in Class Compliant mode.
Its first functional milestone includes compact submix routing, gain, phantom
power, Hi-Z, faders, mute, pan, stereo, meters, EQ, dynamics, AutoLevel, FX,
Room EQ, Control Room, clock settings and DuRec transport. Stereo pairs use a
single strip and fader with two meters, and collapse repeated channel names to
compact labels such as `Analog 1/2`, `Mic/Inst 9/10` and `AES L/R`. Each source
strip places Pan/Balance, Mute and submix-local Solo directly above its meters;
hardware-output Solo is intentionally disabled to match TotalMix FX semantics.
Input and hardware-output strips also expose a compact EQ button beside the
fader. It opens the channel EQ editor directly after that strip, inside the
same mixer row. Later strips move to the right and remain available through the
extended horizontal scroll range; the editor is clipped to its row and never
covers Playback or
another channel. The editor follows the compact TotalMix layout: a 20 Hz–20 kHz
response graph, three colored band columns with type, gain, frequency and Q
controls, followed by the Low Cut controls. Playback EQ remains disabled because
the device does not expose those controls for playback channels.

Every DSP-capable input and hardware-output strip also provides a **Dyn** button.
It uses the same inline-editor behavior as EQ and opens a compact Dynamics panel
with Compressor, Expander and AutoLevel controls. The inspector keeps a
synchronized Dynamics page with a new-window icon for a larger, independently
resizable editor. Clicking that icon again hides the detached window. Playback
strips omit Dyn because the UFX+ does not expose
channel Dynamics for software playback channels.

The Dynamics editor follows the compact TotalMix layout: a graduated input/output
transfer graph shows the Expander region in green, the linear region in blue
and the Compressor region in red, with white markers at the compressor and
expander thresholds. Both axes run from -60 to 0 dB; the green segment enters
at its -60 dB intersection so Expander Ratio changes remain visible. A live
white-ring point follows the loudest selected-channel input peak along that
transfer curve. The adjacent meter block shows the input level and compressor
gain reduction in the inline, inspector and detached layouts. Input is
graduated from 0 to -60 dBFS. Gain reduction has its own precise 0 to -20 dB
scale, with one-dB minor divisions and adaptive major labels. The EQ graph has
the same graduated input meter, including separate lanes for stereo pairs.
Gain, Attack and Release sit below the graph, followed by paired
Compressor/Expander controls and a separate Auto Level section. The detached
window uses a narrow vertical format and reserves roughly one third of its
content height for the graph.

Double-clicking a non-control part of an embedded EQ or Dynamics panel opens
its detached window. Knobs, switches, dropdowns, parameter graphs and other
controls keep their own double-click behavior and never trigger detachment.

When Dynamics is enabled, each input or hardware-output meter includes a narrow
light-blue gain-reduction lane and position marker. Native
`/dynamics/meter` values take precedence when the engine supplies them. The
UFX+ does not publish that UCX II register during a CC refresh, so its display
falls back to the attenuation measured between the pre- and post-DSP peak
values already present in the regular level stream. That fallback uses each
channel's current Attack and Release values for its envelope instead of a
fixed display decay; changing either control therefore changes the meter
ballistics. Native hardware GR remains unfiltered. Meter-only
traffic never rebuilds the inspector controls and is suspended together with
the audio meters while the window is minimized. Stereo hardware outputs keep
both GR lanes visible even if the device reports the Dynamics-enabled state on
only one member of the pair.

Double-click reset to nominal 0 dB is accepted only on the fader handle or its
vertical track, never on empty strip space, meters or adjacent controls.
Double-clicking Pan/Balance recenters it, both in a mixer strip and on the
inspector slider.

The UFX+ CC register dump omits the mixer matrix. The frontend persists every
input and playback send that it controls and restores those values at the next
launch. Before the first saved state exists, the standard 24-channel direct
Playback-to-Output routing is displayed at 0 dB. Restoring this display cache
does not send route changes to the hardware.

A large interlocking-ring symbol below the fader marks every active stereo pair
without consuming space from Pan/Balance, Mute, Solo or the meters.

The selected channel's EQ is also available from the inspector's EQ tab. Its
new-window icon opens a synchronized, independently resizable EQ window;
clicking the icon again, or closing that window, hides it so it can be reopened
without rebuilding the controls. The detached layout expands the graph and
uses larger, more widely spaced controls.

The header-bar inspector button remains accessible at all times and collapses
or restores the complete right-hand inspector without affecting detached DSP
windows.

Room EQ provides a per-pair **Link** control for stereo hardware outputs. Link
is enabled by default and mirrors every Room EQ change to both output channels.
Disengage it to expose the **L** and **R** selectors and edit either side
independently. Re-enabling Link copies the currently selected side to its mate
before subsequent changes are mirrored.

The Room EQ tab has the same new-window toggle as channel EQ and Dynamics. It
opens a synchronized, independently resizable editor for the selected hardware
output. The detached layout provides a larger graph and all nine Room EQ bands.

Channel EQ graphs display three numbered nodes and Room EQ graphs display nine.
Every node uses the same color as its band's controls. Drag a node horizontally
to change frequency and vertically to change gain. Select a node, then use the
mouse wheel over the graph to adjust its Q value without holding the button.
Double-click a node to reset that band's gain to 0 dB. Knob
double-click reset is applied only after a
completed click, so a quick click-and-drag cannot jump to the reset value.
Hover an EQ type selector and use the mouse wheel to cycle through its filter
types without opening the list.

Hold **Ctrl** while dragging an EQ knob, Low Cut control or graph node for
one-tenth of the normal adjustment sensitivity. **Ctrl + mouse wheel** also
uses fine adjustment on continuous EQ controls; for Q it selects the device's
finest 0.1 step instead of the normal 0.2 step.

The renderer is selected by GTK/GSK. OpenGL or Vulkan is used when available;
the application remains usable with GTK's software fallback. Meter rendering
is suspended while the window is minimized.
