# Living World native Viewer adapter 0.1

The mature C++ Viewer's native universe floater can display actual Storm OGRE
Metal pixels in its viewport. The renderer runs in its own process and owns no
world token, model, policy, entity registry or persistent world writer.

```sh
cmake -S living-world -B /absolute/new-render-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/fwd-prefix \
  -DSTORM_SOURCE_ROOT=/absolute/accepted-storm-checkout \
  -DSTORM_OGRE_BUILD_DIR=/absolute/pinned-ogre-build
cmake --build /absolute/new-render-build --parallel 2
ctest --test-dir /absolute/new-render-build --output-on-failure
```

The source and OGRE archives must match `storm-adapter.lock.json`. The tested
source API is the FWD triangle-surface path on Storm main
`d611f7fbf95262dad88140460745e9faf1560e85`. This adapter does not consume the
concurrent Storm Render IR work or the broken prefab cube path.

The tested OGRE archives are an owner-private, stable local rebuild snapshot;
their exact checksums differ from the earlier release archives. The vendored
source matches the accepted main, and the actual Metal pixel gate was rerun
against these copied inputs. Reproduction needs those exact inputs, or an
explicitly reviewed new lock and fresh rendering validation after a rebuild.
Never point this build at another session's changing build directory.

`finalverse-living-render FWD CAS MEDIA NEW_OUTPUT FRAMES visible|hidden`
validates the canonical FWD/CAS and renders bounded triangle geometry.
Optional `FOCUS_E FOCUS_N FOCUS_U SOURCE_REVISION` arguments set the explicit
local camera origin and bind a Viewer stream to its source snapshot revision.
2–36000 scheduled frames, at most60Hz, and four offscreen captures/second.
The frame counter excludes capture's additional internal render submission.
CPU submission timing includes vsync; it is not a GPU completion measurement.

The Core pilot `scripts/omega-xiv/view.py` configures the already-built mature
Viewer with `FINALVERSE_STORM_FRAMES` and an exact
`FINALVERSE_STORM_DEFINITION_SHA256`. Its ordinary read-only SDK connection
still supplies real residents and WorldLine references. The optional frame
bridge checks private-directory permissions, regular files, fixed1280×720 PPM,
SHA-256, world identity, exact source revision and≤2s freshness. It clears bad
or expired frames and never substitutes geometry inspection for pixel output.

Controls in the active floater: **F** or Focus resident; **Home** for overview;
**O** or Orbit; left/right arrows for yaw; mouse wheel for zoom. Camera controls
contain four finite bounded scalars in an owner-private file. They are render
presentation only. The pilot supplies Elara's explicitly registered local
position as view origin. The unvalidated grid/vertical alignment is stated in
the panel; the registration is not a surveyed placement or collision certificate.

The CPU pixel bridge is a bounded integration candidate, not zero-copy GPU
embedding. Resident bodies use published rest geometry, not skinned animation.
A changed world revision requires recomposition/restart of this first renderer
adapter; until then the Viewer clears its outdated image. Aetheria's resident
identities/history remain inspectable and Montréal pixels cannot appear there.
The existing native renderer remains available when this optional feature is
not configured. Only macOS arm64 was executed.
