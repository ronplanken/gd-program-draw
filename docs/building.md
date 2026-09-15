# Building and validation

## Multi-platform pipeline

The `.github` workflows, composite actions, package scripts, installer, CMake
helpers and dependency pins are adapted from `ronplanken/gd-scene-tree` at
commit `200ebe948575e8ca73bc9971b6a3266438d65e8a`. `buildspec.json` is the source
of the package name, version and publisher metadata.

CI runs the three CPU drawing suites on macOS Universal, Windows x64 and
Ubuntu 24.04. Version tags must match the buildspec version and create draft
releases. Repository secrets use the same names as GD Scene Tree; they are
optional for unsigned/ad-hoc packages and are not copied between repositories.

## Installed-OBS development build

Passing `OBS_SOURCE_DIR` selects `cmake/local-obs.cmake`, retaining the original
macOS build against the installed application and its GPU integration test.


The build links against the Qt, libobs and frontend frameworks inside the
installed OBS application. Homebrew supplies headers only; its Qt runtime is
not loaded into OBS.

```sh
cmake -S . -B work/build -G Ninja \
  -DOBS_SOURCE_DIR=/path/to/obs-studio-32.2.2 \
  -DSIMDE_DIR=/path/to/simde \
  -DOBS_APP=/Applications/OBS.app \
  -DQT_HEADERS_PREFIX=/opt/homebrew/opt/qtbase \
  -DQT_SVG_HEADERS_PREFIX=/opt/homebrew/opt/qtsvg \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build work/build --parallel 4
ctest --test-dir work/build --output-on-failure
```

`scripts/build-macos.sh` fetches the pinned header revisions automatically.
The script does not compile or replace OBS.

## Tests

- `canvas-behavior`: monitor geometry, letterboxing, pixel ratios, snapshots,
  undo, clearing and concurrent snapshot access.
- `tools-and-layers`: drawing tools, selections, images, layer order,
  visibility, opacity and history.
- `sports-telestrator`: analysis tool geometry, marker sequencing and the
  lifecycle of held Highlighter and Spotlight effects.
- `libobs-render-output`: actual composited video pixels in a separate libobs
  process, using synthetic video without a streaming-service connection.

The video test requires access to the macOS graphics session. The tests do not
establish compatibility with other OBS versions or platforms, nor do they
replace testing an encoded recording or real broadcast.

## Icon assets

The Lucide SVG sources are vendored at a pinned revision in `third-party/lucide`.
Regenerate the embedded header after changing the icon mapping:

```sh
python3 scripts/embed-lucide.py
```

The plugin renders icons with OBS's QtSvg. Filled and elliptical variants use
the upstream geometry with a fill or vertical scale. The build copies the
Lucide licence into the plugin bundle before signing it.

## Output integration

The plugin claims a free high-numbered main-output channel for a private overlay
source. It leaves program channel zero and occupied channels intact. Artwork
is rendered to a CPU image and uploaded only when the image revision changes.

The input adapter locates the existing preview/program widget and adds an event
filter and toolbar. It depends on OBS's internal widget structure and a checked
Qt call for normal-mode preview fitting. Review this adapter before upgrading
OBS. Input never comes from a second video dock.
