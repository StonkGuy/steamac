# KosmicKrisp patches

`build.sh` (one level up) applies these `git format-patch` files in name order on top of the pinned Mesa commit.
Each patch names its upstream origin (a Mesa merge request) or says it is a steamac change.

The series is numbered 0001–0044, then 0046, 0047. **0045 is intentionally absent**: it was a draft that was withdrawn
before it was committed, so no patch carries that number and nothing depends on it. The gap is kept so that numbers
cited in older notes still refer to the same change.

Patch 0041 (asynchronous pipelines) is opt-in at run time (`MESA_KK_ASYNC_PIPELINES`, off by default). See
`docs/fork.md` for its status.
