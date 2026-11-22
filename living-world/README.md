# Living World native Viewer adapter 0.2 — Ω-XV

This source-pinned C++ adapter produces actual OGRE Metal frames for the maintained
C++ Viewer. It is a CPU captured-frame stream: offscreen GPU readback, private PPM
files and Viewer texture upload. It is not shared GPU surfaces or a replacement
Viewer rendering backend. The child has no world token, model or persistent writer.

## Frozen dependencies and build

The source lock distinguishes accepted Storm main `d611f7fbf95262dad88140460745e9faf1560e85`
from the frozen local API candidate `6a4f99207f3eee9b817ef51ddf7ef3517118a5e4`.
The latter adds surface handles, transforms, materials and removal. It is not called
an accepted Storm release. Exact renderer/FWD source files and seven private stable
OGRE archive/header inputs must match `storm-adapter.lock.json`. Never build against
another session's changing working tree or build directory.

```sh
cmake -S living-world -B /absolute/new-render-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/fwd-prefix \
  -DSTORM_SOURCE_ROOT=/absolute/frozen-storm-checkout \
  -DSTORM_OGRE_BUILD_DIR=/absolute/private-pinned-ogre-inputs
cmake --build /absolute/new-render-build --parallel 2
ctest --test-dir /absolute/new-render-build --output-on-failure
```

`-DLIVING_SANITIZERS=ON` instruments the local admission/control/extension code and
unit tests with ASan/UBSan. It does not claim instrumentation of prebuilt OGRE.
`-DLIVING_FUZZ=ON` adds `living-bridge-fuzz` for Clang libFuzzer, ASan and UBSan.
LLVM22 needs delayed parsing for an unused ill-formed assignment in the pinned legacy
RapidJSON template; the narrow fuzz translation-unit flag does not patch the vendor
source or substitute a different parser. Release and sanitizer builds use Apple Clang.

## Execution and camera

```sh
finalverse-living-render FWD CAS MEDIA NEW_OUTPUT FRAMES visible|hidden \
  [FOCUS_E FOCUS_N FOCUS_U [SOURCE_REVISION [REGION_UUID]]]
```

Canonical FWD/CAS hashes, strict GLB profile, explicit metre axes and double-precision
rebasing are validated before any surface admission. Physical positions remain in the
FWD's east/grid-north/up frame. Initial view origin is optional and independently
bounded. 2–36000 scheduled frames, at most60Hz, approximately4 captures/s,1280×720.
Frame count excludes each capture's additional internal render submission; CPU submit
measurements include blocking/vsync and are not GPU completion timing or FPS claims.

The atomic owner-private `camera.json` contract is:

```json
{"schema":"finalverse.presentation-control/0.1","yaw":0.4,"pitch":-0.4,
 "distance":200,"orbit":false,"pan":[0,0,0],"lighting":"daylight",
 "target_resident":"904d952a-aca7-51da-aa01-20aab7dd9893"}
```

`target_resident` is optional/null and resolves only against current validated
resident identities. Pan is a bounded presentation offset in the registered FWD
frame. Yaw, pitch, distance, focus and pan interpolate smoothly; orbit advances
rather than resetting on repeated reads. Legacy four-scalar `camera.txt` remains
available when JSON is absent. Inputs use no-follow file opens, regular same-user
files,0600 permissions and bounded sizes. Unknown/non-finite values are refused.
Directional light, scalar PBR material factors and ambient presets are implemented.
The gradient sky and horizon matte are explicitly authored backdrop geometry,
not new surveyed terrain, physical atmospheric scattering or image-based lighting.

## Live committed resident state

Pass an independently pinned region UUID to enable `scene-update.json`. The parent
native SDK consumer emits `finalverse.scene-update/0.1` with world/region/epoch,
cursor, revision/previous_revision, full_snapshot, pinned definition hash,
residents, assets and WorldLine event references. This is an adapter document,
not another FVP wire protocol. The parent remains responsible for verified FVP
admission, gap recovery and real geographic transformation.

This adapter requires a full desired resident set, at most16 entries. Canonical FWD
simulation bindings and `resident-` provenance identify known body assets. A body
must have an unambiguous content SHA and stable entity/resident identity. New poses
move existing surface handles; absence removes a known body; reappearance restores
its already-admitted mesh. Unknown bodies or geometry cannot be loaded through an
update. `assets` must be empty; changed static geometry requires an explicitly
admitted FWD reload. Static surfaces remain pinned to the original FWD/CAS.

Duplicate cursor/payload is a no-op. Conflicting cursor payload, revision regression,
changed state at the same revision, wrong world/region/definition, malformed/deep
JSON, unknown fields or excessive budgets are refused. A new epoch or revision gap
requires a full snapshot; the parent consumer verifies recovery before sending it.
The adapter records snapshot resynchronization separately from ordinary updates.

Frame metadata includes applied revision/cursor/epoch, exact update hash,
`registered_residents`, `focus_origin` and `interpolation_active`. Root movement is
smoothed between committed positions; it is not prediction or a world mutation.
Displayed positions are explicitly interpolated presentation. No skeletal skinning,
walk cycle or decorative idle behavior is claimed. Reconciliation waits for the
actual target revision and completed interpolation. The Viewer validates expected
FWD/pixel hashes, world/revision and freshness≤2s and clears stale images.

## Optional source-pinned PSSM capture extension

`FINALVERSE_STORM_SHADOWS=1` enables `shadows.cpp`. Default is off. Through public
OGRE APIs only, it configures the frozen named capture pass to use the stock
`PbsMaterialsShadowNode`: three directional PSSM splits,600m shadow distance,
2048×7168 depth atlas and a1024 cube temporary. The authored sky/horizon do not cast
shadows. Its resource use is larger than the no-shadow path and is not a GPU-memory
certification. Do not enable on another source revision without compatibility tests.

Each offscreen capture creates a fresh workspace and receives this shadow definition.
The existing hidden renderer window's privately owned workspace is unchanged; no
private fields/macros or source rewrites are used. The actual Viewer sees the captured
shadowed pixels. This is a local capture-adapter feature, not a claim that Storm's
public renderer interface supports shadows. Ray tracing, HDR tone mapping, textures,
IBL, physical atmosphere, temporal AA and GPU instancing remain unavailable here.

## Actual pixel fixture and evidence

```sh
python3 living-world/test_live_pixels.py \
  --renderer /absolute/new-render-build/finalverse-living-render \
  --fwd /absolute/real-composed/world.fwd.json \
  --cas /absolute/real-composed/objects --media /absolute/ogre-media \
  --out /absolute/new-pixel-fixture
```

This GPU fixture intentionally supplies synthetic presentation poses, not world events.
It verifies changed body pixels without restarting the renderer, known-body removal
and restoration, unknown-hash refusal, full-snapshot gap recovery, typed lighting and
smooth target camera/pan. It does not replace the authoritative action→FVP→Viewer test.

Local evidence under `dev/omega-xv-20261119`: Release2/2 CTests, ASan/UBSan2/2,
`render-live-fixture-1/result.json`, `render-shadow-verification.json`, and bounded
parser fuzz results. Same final showcase source/camera with shadows on/off changed
13.1968% pixels by more than3/255; actual cast shadows were visually reviewed.
Initial rejected shadow workspace-listener experiments are retained as failures.
Only macOS arm64 was executed. Read the Core Ω-XV acceptance report for the integrated
world test, current exact source/binary hashes and unresolved experience gates.
