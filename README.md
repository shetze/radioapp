# Sample automotive applications

This directory contains a set of applications that demonstrate how
typical automotive applications use SOME/IP to talk to each
other. These apps work both natively and in a container.

## The apps

### engine-service

This is a very simple service with a field event that signals whether
the car is reversing, published initially and on direction transitions.

### radio-service

This is a service that emulates a radio, regularly publishing
information about the current song, radio station and volume. It
accepts requests to turn on/off, switch channel, and change volume.
If the engine service is available it will listen for events from it
and temporarily lower the volume while reversing.

### sensors-service

This independent simulation service listens to engine direction and generates
rear-view camera frames plus four ultrasonic distances over reliable SOME/IP.
It requires no actual camera or ultrasonic hardware. See below for the wire
format, configuration and safety limitations.

### radio-client

This is a command line program that displays the current state of the
radio service, as well as allow you to control it. The keyboard controls
are displayed on the screen. `V` opens a full-screen local ASCII/ANSI video
library with a minimum display size of 100x30 characters. Select animations
with the arrow keys and Enter. Playback loops until `X` returns to the video
selection; `V` returns directly to radio, and `Q` quits. `ESC` returns to the
selection while playing and to radio from the selection. With Reverse active,
the camera preempts the monitor or video library, but not the basic radio view.

## Building

The apps depend on Boost, vsomeip3, ncursesw and libcaca (the development
package is `libcaca-devel` on Fedora/EPEL). Build with CMake:

```
 $ cmake .
 $ make
```

There is also a makefile that allows building rpms and srpms:

```
 $ make -f Makefile.rpm srpm
 $ make -f Makefile.rpm rpm
```

## ASCII/ANSI videos

libcaca decodes multi-frame `.caca`, modern ANSI/UTF-8 `.ansi` / `.ans`
(form-feed + newline frame separators), single-frame text, and directories of
sorted ANSI frames. The radio-client renders the decoded frames into its own
full-screen ncursesw window, without starting another terminal application.
The input size is **not** fixed at 100x30: the terminal must provide at least
100x30 characters **and** be at least as large as the selected animation's
native dimensions. Smaller videos are centred. Files are loaded on selection;
input size and frame count are bounded.

The default library is `${CMAKE_INSTALL_DATADIR}/auto-apps/animations`
(e.g. `/usr/share/auto-apps/animations` in an RPM installation). The bundled
`city-traffic.ansi` is a complete 10-second demo, 100x30, 10 FPS.

To use a different local library, set:

```sh
export RADIO_VIDEO_DIR="$HOME/animations"
radio-client
```

A frame directory can contain `0000.ansi`, `0001.ansi`, etc. Playback defaults
to 10 FPS, but a plain text `.fps` sidecar containing an integer between 1 and
60 overrides this. For a file, use `my-video.caca.fps`; for a directory, use
`animation.fps`. See `animations/README.md` for details.

In video mode, the radio's SOME/IP connection and telemetry continue running.
Video playback does not issue volume, station or engine commands. Older
Durdraw `.dur` files should first be exported to ANSI; the native `.dur`
format is not supported.

## Simulated rear camera and parking sensors

`sensors-service` is a **simulation only**: it does not open a camera device or
read real hardware. It listens to the engine's reverse field event via SOME/IP
and publishes a deterministic rear-view ASCII/ANSI animation together with four
matching ultrasonic distance readings. Engine-service remains authoritative for
reverse; sensors-service never changes vehicle direction.

The radio-client uses the following display priority:

| Engine | UI state | Visible content |
| --- | --- | --- |
| Forward / Unknown | Radio, monitor closed | Radio |
| Reverse | Radio, monitor closed | Radio (no camera) |
| Forward / Unknown | Monitor expanded (`M`) | Radio + interference monitor |
| Reverse | Monitor expanded (`M`) | Full-screen simulated rear camera |
| Forward / Unknown | Video library/player (`V`) | Local animation / browser |
| Reverse | Video library/player (`V`) | Full-screen simulated rear camera |

Local animation playback **pauses**, rather than restarting, while the camera
has priority. On leaving reverse, the same frame and the selection state are
restored. While reverse is active, `V` may close an open video library and `M`
may collapse an open monitor; `X` cannot dismiss the automatic camera overlay.
`Q` still exits. The simple radio display never forces the camera open.

Three versioned, independent SOME/IP field/eventgroups are provided by service
`0x1003`, instance `0x0001`:

| Event | Eventgroup | Meaning / wire format |
| --- | --- | --- |
| `0x8001` | `0x4005` | `CAM1`: camera frame with width, height, epoch, sequence, printable ASCII/color cells |
| `0x8002` | `0x4006` | `ULT1`: four ultrasonic distances in mm, validity mask, epoch, sequence |
| `0x8003` | `0x4007` | `CST1`: inactive/active/fault, epoch and sequence (1 Hz heartbeat) |

**Transport:** The sensor instance uses reliable SOME/IP (TCP port `30511`)
configured in both `/etc/vsomeip.json` templates in `auto-app.aib.yml`.
A 100x30 camera event contains ~6 KiB of data; it is intentionally **not**
transmitted as a fragmented UDP datagram. The ultrasound event is 5 Hz, and
camera frames are sent only while the engine reports Reverse. Subscribers
should use a sufficiently current vsomeip configuration and compatible event
definitions. Frames use bounded 2-byte cells (printable ASCII + 4-bit foreground
and background ANSI color indexes), rather than transmitting unsafe control
sequences. The client converts them to a libcaca canvas and uses the existing
ncursesw video renderer. Each sensor value is a simulation and must not be used
for real driver assistance or safety functions.

By default the camera runs at **100x30, 10 FPS**. The independent sensors-service
process supports the following environment overrides (restart to take effect):

```sh
SENSORS_CAMERA_WIDTH=160 SENSORS_CAMERA_HEIGHT=50 SENSORS_CAMERA_FPS=15 sensors-service
```

Camera dimensions may be 100..240 columns and 30..80 rows; FPS may be 1..30.
The terminal must fit both the 100x30 minimum and the native camera dimensions.
The overlay displays ultrasonic measurements in millimeters; values under
1500 mm are yellow and under 500 mm red (demo thresholds).

On service loss, fault status, expired status heartbeat (3 s), expired frame
(1.2 s), or expired ultrasonic sample (2 s), the client displays an explicit
unavailable/stale indication. It never reuses the last frame as live video.
A camera epoch prevents frames from an earlier Reverse period from being replayed.

The CMake target, RPM manifest, container entrypoint and AIB QM deployment all
include `sensors-service`. A standalone `sensors.container` Quadlet is also
provided. When deploying on AutoSD, ensure the runtime SOME/IP JSON contains
application `Sensors` (`0x1003`) and the reliable service configuration. Run
`ctest --output-on-failure` after building; `sensors-test` exercises the wire
format, simulation, state transitions, freshness handling and larger frames.
