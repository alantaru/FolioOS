# Security Policy

FolioOS runs with full control of the machine, straight from UEFI firmware, and reads files
from removable drives (documents, fonts, translations, keyboard layouts, themes, MP3s).
Bugs in those parsers are the most likely security issues.

## Supported versions

Only the latest release receives fixes.

## Reporting a vulnerability

Please **do not open a public issue**. Use GitHub's private reporting instead:
**Security** tab › **Report a vulnerability**. Include the FolioOS version, the file or steps
that trigger the problem and, if possible, `FOLIO\DIAGNOSTICS.TXT`.

You can expect an acknowledgement within 7 days. Once a fix is released, you will be credited
in the release notes unless you prefer to stay anonymous.

## Design notes

- FolioOS has no network stack, so remote attacks are out of scope by design.
- Translation files are validated when loaded: a translation whose `printf` placeholders do
  not match the original is discarded.
- Paths from data files (keyboard `include`, language `X-Fonts`) are confined to their folders
  (`..` and absolute paths are rejected).
