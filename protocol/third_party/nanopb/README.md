# nanopb runtime provenance

This directory vendors the minimal C runtime required by nanopb-generated
messages:

- `pb.h`
- `pb_common.c`, `pb_common.h`
- `pb_encode.c`, `pb_encode.h`
- `pb_decode.c`, `pb_decode.h`
- `LICENSE.txt`

The files are unmodified from official nanopb release `0.4.9.1`, Git commit
`cad3c18ef15a663e30e3e43e3a752b66378adec1`. Upstream project:
<https://github.com/nanopb/nanopb>.

No generator, Python package, tests, examples, or platform integration is
vendored. `wlh_nanopb_runtime` / `wlh::nanopb_runtime` exposes these sources as
an opt-in static C99 library. The wire-only `wlh_protocol` target has no nanopb
dependency.
