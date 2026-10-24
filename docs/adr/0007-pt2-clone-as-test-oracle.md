# pt2-clone's replay code is the test oracle for the Replayer

To prove ProTracker 2.3 accuracy (ADR-0002), the PC test suite compiles pt2-clone's replay code with stubs and records every Paula register write it makes, tick by tick. Our Replayer must produce the identical sequence for every test Song. Test Songs are a few real MOD files supplied by the project owner (committed in `tests/songs/`) plus small generated Songs that each exercise one effect. pt2-clone (BSD-3) is a test-only tool and never ships in the firmware.

## Considered Options

- **Compare rendered audio**: hides which effect is wrong, and pt2-clone's audio path adds filtering we don't replicate.
- **Hand-written expected values per effect**: only as accurate as our reading of ProTracker's quirks.
