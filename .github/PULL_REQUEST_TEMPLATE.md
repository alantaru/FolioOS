## What does this change?

## How was it tested?

- [ ] `.\build.ps1` ends with `Done.`
- [ ] Tested on real hardware (model: …)
- [ ] New interface texts are marked with `L()` / `N_()` / `LN()` (no language in the code)
- [ ] User files on the drive are never renamed, moved or deleted
- [ ] If it writes to disks or partitions: refusals leave the disk byte for byte the same, and
      `.\tools\qemu-lab.ps1 -Only "^(inst|dual)"` passes
