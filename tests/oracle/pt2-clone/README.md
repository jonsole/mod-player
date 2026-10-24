# pt2-clone replay code (test oracle only)

Copied unchanged from [8bitbubsy/pt2-clone](https://github.com/8bitbubsy/pt2-clone) at commit `18954f23c3d0ebd8583e61b65fe4507ae8aff4bd` (BSD-3-Clause, see `LICENSE.txt`):

- `pt2_replayer.c`, `pt2_replayer.h`: the C port of ProTracker 2.3D's replay routine.
- `pt2_tables_excerpt.c`: the two tables it uses, copied verbatim from `pt2_tables.c`.

It is compiled only for the PC test suite (ADR-0007), with the editor, GUI and audio headers replaced by `../stubs/`. It never ships in the firmware. To update, copy the files from a newer commit and re-run `make oracle-test`.
