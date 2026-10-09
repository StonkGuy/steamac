# Withdrawn patches

`0021-JIT-do-not-commit-the-RSP-increment-when-pop-r-m-fau.patch` was written to make a faulting `pop r/m`
leave RSP unchanged, and `tests/pop-fault` passes with it. It is withdrawn because the rewrite of the JIT
`pop r/m` **memory-destination** path broke the normal (non-faulting) execution EAC's launcher relies on:
with it installed in Steam's FEX tool, VRChat's EAC launcher hangs at `Starting Wine module mapping` and
never reaches `Launcher finished with: 301`. Bisected live on one VM: `0001–0020` reach 301, `0001–0021`
do not. The series skips 0021 (`0001–0020`, then `0022` onward); this file is kept so the withdrawal is auditable and so
`tests/pop-fault` (kept as a known-fail) has its patch. Re-introduction requires fixing the de-fused
memory-destination `pop` and re-running the live EAC gate, not just `tests/pop-fault`.
