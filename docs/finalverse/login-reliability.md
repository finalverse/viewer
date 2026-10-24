# Login diagnostics and OpenSim overlay bounds

Finalverse login errors now direct inherited Second Life support email and HTTP/HTTPS support links to https://github.com/finalverse/viewer/issues. The normal error resolution still handles localized message IDs and server fallback text; independent grid operators’ contacts remain intact. Copyright and licensing notices are unaffected.

A local 512 m OpenSim region reproduced a post-login heap overwrite. It sent parcel overlay sequences 0–15 into the inherited four-chunk, 256 m overlay. Full GuardMalloc identified the original invalid write at sequence 4 in `LLViewerParcelOverlay::uncompressLandOverlay`. Later avatar-morph and allocator crashes were downstream symptoms. This change checks the sequence at receipt and checks the actual destination capacity and payload length at copy. It does not substitute a guessed region size or claim full extended-region terrain/parcel support.

Regression cases cover all four ordinary chunks, negative/out-of-range/maximum sequence numbers, truncated payloads, inconsistent capacity, null input and leading/trailing canaries. Support-message cases cover localized email, both URL forms and preserving unrelated contacts. All 119 CTest targets pass after the native rebuild; a fresh Ada Research session entered the dedicated local Old Montreal region and completed client baking without the previous startup crash.

Use a First Last avatar login name and the intended grid. Avatar UUIDs are entity identifiers, not login names. Local research credentials are separate from public MutSea and Second Life accounts; keep them outside the repository and command arguments. No public deployment or release is included in this fix.
