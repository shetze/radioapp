# Sample automotive applications

This directory contains a set of applications that demonstrate how
typical automotive applications use SOME/IP to talk to each
other. These apps work both natively and in a container.

## The apps

### engine-service

This is a very simple service with a single event that signals that
the car is reversing, which is regularly emitted.

### radio-service

This is a service that emulates a radio, regularly publishing
information about the current song, radio station and volume. It
accepts requests to turn on/off, switch channel, and change volume.
If the engine service is available it will listen for events from it
and temporarily lower the volume while reversing.

### radio-client

This is a command line program that displays the current state of the
radio service, as well as allow you to control it. The keyboard controls
are displayed on the screen. `V` opens a full-screen local ASCII/ANSI video
library with a minimum display size of 100x30 characters. Select animations
with the arrow keys and Enter. Playback loops until `X` returns to the video
selection; `V` returns directly to radio, and `Q` quits. `ESC` returns to the
selection while playing and to radio from the selection. Radio/monitor operation
is unchanged while video mode is hidden.

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
