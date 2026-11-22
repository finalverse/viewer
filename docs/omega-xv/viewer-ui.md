# Ω-XV native Viewer experience

The native universe floater remains part of the maintained C++ Viewer. The existing UI framework, glTF loader, read-only SDK and application identity are preserved. This is an optional surface of the same application and connection, not a second shell or a Qt prototype.

## Design system and modes

A compact Finalverse header groups Explore, Create and Develop, world selection, theme and connection controls. Dark surfaces use slate/navy and high-contrast pale text; the light palette uses cool gray surfaces and dark text. Existing native buttons and widgets preserve keyboard focus, tooltips and platform DPI scaling. Local theme changes affect this surface only.

Explore starts with a world-wide viewport and keeps diagnostics out of the scene. Info opens a contextual resident/world inspector. Create exposes the same inspector with authored-preview provenance and a plaza preview entry. Develop shows source frame, world revision, cursor, published object selection, render counters and WorldLine information. It displays the full captured image; Explore/Create use aspect-preserving crop rather than geometric stretching. The window resizes down to960×640 logical pixels, with secondary actions reduced in the compact layout.

The source viewport is the actual Storm1280×720 capture stream. It is read back and uploaded to a Viewer UI texture. This is not shared GPU-surface rendering. Revisions, scope, expected definition digest, pixel digest and ≤2s freshness are checked before displaying a frame. Full integration evidence belongs to the Core Ω-XV acceptance report.

## Navigation and inspection

- Ctrl+1/2/3 (verified on macOS): Explore/Create/Develop.
- F: focus the selected canonical resident.
- Home:600m overview, reset pan/yaw/pitch and disable orbit.
- O: toggle orbit; left/right: yaw; up/down: pitch.
- WASD: bounded planar camera pan; wheel: bounded zoom.
- Lighting selector: explicit daylight/sunset presentation preset.

Explore initially requests200m distance and−0.35 pitch rather than a distant GIS overview. Focus uses12m. The renderer smoothly interpolates its presentation controls. Focus passes the selected resident UUID to the renderer; it never guesses geographic transforms from raw SDK coordinates. Inspector local Z-up coordinates and separately admitted displayed registered E/N/U coordinates are labeled distinctly; the latter may include presentation interpolation toward committed state. Full world/region/object UUIDs are available in tooltips; canonical resident and entity UUIDs remain visible in the inspector.

Resident selection is a native combo box of actual SDK identities. Published-object selection lists committed SDK entity IDs. No screen-space hit-testing or selection of arbitrary non-authoritative decorative geometry is claimed. Existing Aetheria world selection remains available and cannot reuse Montréal pixels.

## Bounded presentation commands

The Ask entry recognizes explicit local commands: daylight, sunset, focus, “move the camera closer to Elara”, cinematic, “use a cinematic camera view”. A named Elara focus refuses when she is absent from the current world. Presentation commands change only camera/lighting files and cannot mutate world state.

The worker owns interaction transport. Unsupported free-form text is sent as a bounded observation request for the owning service to interpret/refuse. Text is limited to1024 UTF-8 bytes. There are no UI subprocesses, shell commands or model calls.

Camera files live only in the configured private `FINALVERSE_STORM_FRAMES` directory. `camera.json` uses `finalverse.presentation-control/0.1`: yaw, pitch, distance, orbit, bounded pan, daylight/sunset and optional target_resident. It is atomically replaced. The four-scalar camera.txt remains a legacy fallback. These are local presentation controls, not a new world-state protocol.

## Reviewed world interactions

`FINALVERSE_WORLD_INTERACTION_DIR` is a configured absolute owner-private directory. The UI serializes a bounded request and queues it; the existing observation worker writes request.json atomically and reads bounded status.json. It does not obtain a write capability from model text.

Request schema `finalverse.viewer-request/0.1` contains id, world, region, resident, source_revision as a decimal string, operation and arguments. The current operations are:

| Operation | Arguments | UI entry |
| --- | --- | --- |
| resident.observe | request bounded text | Ask |
| resident.move | direction EAST or WEST | Plan east/west or exact text |
| resident.communicate | text | “say …” |
| resident.interact | target committed entity UUID | Select object, then “interact” |
| scene.preview | request bounded text | Preview plaza or explicit creation/preview text |
| proposal.approve | proposal_id | Approve reviewed plan |

The owning service returns `finalverse.viewer-status/0.1`: id, world, region, stage, detail, committed_revision decimal string, command_id and optional proposal_id/requires_approval. Pending, accepted, rejected, committed and reconciled are shown separately. A preview is not a commit. A status must match the exact request ID and world/region. The Approve button is enabled only for an accepted concrete proposal, explicit approval requirement and the same selected resident. Submitting a request immediately retains pending state to prevent double-click races. A channel failure reports unknown commit status and never invents an authoritative rejection or success.

The Approve button sends a new request referencing the displayed proposal. The owning service must check the proposal, authorization, world/revision, publication and commit. UI natural language cannot approve its own mutation. Disconnect clears current state and disables approval; reconnect must recover actual authority state.

## Validation and limits

Static checks parse the XUI and verify that each named C++ control exists; all32 named controls are present. Source checks confirm that no subprocess, shell or inference execution is introduced. Actual C++ compilation, keyboard focus behavior, UI readability and live movement/reconnect validation require the native application exercise and are recorded by the integration owner.

This change does not replace the application's entire legacy skin, animate mode transitions, implement ray picking, introduce a global icon font, or provide zero-copy rendering. Geographical accuracy, broker authorization, renderer quality and scene enrichment have independent owning contracts. An attractive UI does not certify those properties.

## Reload of an approved publication

The optional `FINALVERSE_STORM_ADMISSION` points to an absolute regular0600 JSON file in an owner-private directory. Reads use `O_NOFOLLOW`, same-UID verification and a4096-byte bound. `finalverse.render-admission/0.1` has exactly eight fields: schema, world UUID, region UUID, definition_sha256, source_entity UUID, publication_version SHA256, command_id UUID and committed_revision as an unsigned decimal string.

The observation worker checks admission after decoding the actual SDK snapshot. The world/region must match, current revision must be at least the committed revision, an actual non-resident SDK asset must reference exactly that entity and publication version, and the publication's command UUID must occur in actual SDK WorldLine event command references. Only then can the expected FWD digest change. Initial environment pinning remains the fallback; malformed files, wrong scope, missing assets/versions, future revisions and unproven new commands cannot admit a new digest.

An accepted immutable record is retained for this connection, so a rolling recent-history window does not revoke already-proven admission. Its world/region, revision and asset/version binding are rechecked on every snapshot. Disconnect discards that memory; a new connection must prove admission again. The owning service controls checkpoint/history retention needed for that proof. A replacement admission must pass the full check unless it exactly matches the retained record. This is a trusted same-user presentation association, not a new world-state writer or production authentication boundary.

On a valid change, the frame must match the newly admitted digest and world and the current exact SDK revision. Image uploads key on both digest and frame number, so a restarted child cannot accidentally reuse a same-numbered cached image. The renderer may restart independently while the Viewer and native world connection remain open.

The renderer's interpolation_active flag is presentation-only. Even if the broker reports reconciliation, the visible UI remains COMMITTED until a matching current source revision is displayed with interpolation inactive. Displayed registered resident positions are explicitly distinguished from authoritative SDK positions during that transition.

Final desktop audit: compact coordinates and revision remained visible; full movement review fit before approval. Ctrl mode shortcuts, local sunset/focus, light/dark themes and wrong-world pixel clearing were exercised. The old Command-key hint was corrected to the observed Ctrl behavior. Sunset is too dark for visual acceptance.
