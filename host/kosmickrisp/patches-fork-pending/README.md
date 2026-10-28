# KosmicKrisp patches of this fork waiting to be re-ported onto fxgl/steamac's series

fxgl/steamac 1.9.2–1.9.4 extended the shared KosmicKrisp series (0001–0040) with its own 0041–0063.
This fork's 0041–0049 used the same numbers and partly the same areas, so they are kept here, out of the
build, until each one is re-ported on top of 0063 (renumbered from 0064) and measured again.

| patch | what it does | relation to the upstream series |
|---|---|---|
| 0041, 0043, 0047, 0049 | MESA_KK_ASYNC_PIPELINES: Metal render pipeline states built on a worker pool (opt-in), a failed state drops its draws, the draw gate follows the program a draw uses, workers joined before the compiler is released | upstream's 0051–0057 compile a pipeline's programs on a thread pool, share states and compile a queued program at its first use: likely supersedes these; re-port only if measured better |
| 0042 | robustness fixes in 0010, 0019, 0027, 0031, 0036 (all in the shared 0001–0040): XFB counter bounds, plane layouts without a check, sparse container_of, minmax update-after-bind scan, sparse view underflow | not covered upstream: re-port first |
| 0044 | wires Mesa's on-disk shader cache | upstream's 0055 keeps translated shaders in a device cache and on disk: compare, probably drop |
| 0046 | bounds the indirect-deref branch tree | not covered upstream: re-port |
| 0048 | texel buffer views of formats without texel buffer support read as null descriptors | upstream's 0042 does this and adds R32G32B32 uniform texel buffers (this fork left that part out as a suspect for a Unity HDRP GPU hang): re-test that title on upstream's 0042 |

Tests in host/kosmickrisp/tests/ (async-*, texel-view-unsupported, indirect-io) belong to these patches.
