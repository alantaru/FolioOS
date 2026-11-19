# Contributing to FolioOS

Thank you for helping! FolioOS is a small project with a clear purpose — a distraction-free
writing machine that anyone can boot from a USB drive — and every kind of contribution
matters: bug reports from real hardware, translations, keyboard layouts, themes, fonts,
documentation and code.

## Ways to help

| You want to… | Start here |
|---|---|
| Report a bug | [Open an issue](https://github.com/alantaru/FolioOS/issues/new/choose) and attach `FOLIO\DIAGNOSTICS.TXT` from the drive. |
| Report how FolioOS runs on your PC | Use the **Hardware report** issue form — even "it just works" reports are valuable. |
| Translate or fix a translation | [docs/TRANSLATING.md](docs/TRANSLATING.md) |
| Add a keyboard layout | [docs/TRANSLATING.md › Keyboard layouts](docs/TRANSLATING.md#6-keyboard-layouts) |
| Share a theme | Add an `.INI` file to `assets/themes/` (see `Blue Ink.INI`). |
| Propose a feature | Start a [Discussion](https://github.com/alantaru/FolioOS/discussions) first. |
| Write code | Read on. |

## Development setup

- Windows 10/11, PowerShell 5.1+, [MinGW-w64](https://winlibs.com) `gcc` (x86_64) on `PATH`.
  The ARM64 and BIOS targets also use [LLVM](https://llvm.org) (`clang`, `ld.lld`) and `nasm`;
  the hardware lab uses [QEMU](https://www.qemu.org). All three install with `winget`.
- `.\build.ps1` builds everything and runs the full test suite (simulator, real UEFI binary
  inside `efi_host`, exports, translations, partition tables, installer, file-system checks).
  It must pass before a pull request is merged; CI runs it on every pull request.
- `.\tools\qemu-lab.ps1` boots the real binaries on 20 emulated machines (`-List` shows them,
  `-Only <regex>` runs some). Set `FOLIO_WORK` to a folder on a roomy disk if `C:` is short on
  space: the lab and the disk-image tests write large images there.
- `.\build\folio_sim.exe` is the fastest way to work: the same core code in a window.
- On a Mac, `./build_macos.sh --test` builds the simulator and runs the same scripted tests.

## Code guidelines

- **Freestanding C.** `src/core` has no libc; use the helpers in `folio.h` (`fmt`, `str_*`,
  `mem_*`, `Buf`). Platform code goes behind the `pal_*` interface.
- **No language in the code.** Every user-visible text is English and marked:
  `L("…")` at the point of use, `N_("…")` in static tables, `LN("one", "many", n)` for plurals.
  Folder names come from `paths.c`, keyboard behavior from layout files. The build regenerates
  `assets/i18n/folio.pot`; run `build\i18n.exe update` to merge new texts into the `.po` files.
- **The USB drive stays plain FAT32.** No hidden partitions, no proprietary formats; user files
  are never renamed, moved or deleted by updates.
- **Never lose text.** Saves are atomic, drafts are recovered after power loss; keep it that way.
- **Code that writes to disks** (`part.c`, `install.c`, `fscheck.c`, `fat32.c`) follows the rules
  in [ROADMAP.md › Rules of the partition manager](ROADMAP.md#rules-of-the-partition-manager-both-editions):
  back up before writing, order writes so a power cut leaves a usable disk, and refuse whatever
  cannot be verified. Every refusal needs a test that the disk stays byte for byte the same, and
  file-system changes are checked by the owning system (Windows `chkdsk`, Linux `fsck`) on the
  fixtures in `tests/fixtures`.
- **Tests.** New behavior gets a scripted test in `tests/` (see existing scripts and the
  commands in `src/platform/win32/win32_main.c`). For a new check, show that it fails without
  the change (a negative control) before relying on it.
- Keep the existing style: compact functions, comments explaining *why*.

## Pull requests

1. Fork, create a branch (`feature/…`, `fix/…`, `i18n/<code>`).
2. Make focused commits with clear messages (imperative mood: "Add Hindi translation").
3. Run `.\build.ps1` and make sure it ends with `Done.`
4. Open the pull request using the template; describe how you tested it, ideally on hardware.

By contributing you agree that your contributions are licensed under the
[GPL-3.0](LICENSE), the license of the project.

## Community

Be kind and patient — see the [Code of Conduct](CODE_OF_CONDUCT.md). Maintainers can be
reached through GitHub issues and discussions.
