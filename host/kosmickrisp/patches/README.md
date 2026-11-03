# KosmicKrisp patches

`build.sh` (one level up) applies these `git format-patch` files in name order on top of the pinned Mesa commit.
Each patch names its upstream origin (a Mesa merge request) or says it is a steamac change; `build.sh` describes
every patch of the series. The series is upstream steamac's; numbers 0060 and 0070 are not used.

This fork's own KosmicKrisp patches 0041–0049 predate upstream's use of those numbers. They are parked out of the
build in `../patches-fork-pending/`, with their status against this series. Patch 0041 there (asynchronous
pipelines, `MESA_KK_ASYNC_PIPELINES`) is one of them. See `docs/fork.md`.
