# Third-party notices

FolioOS itself is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It bundles or downloads the following third-party components, each under its own license.

| Component | Use | License | File |
|---|---|---|---|
| [minimp3](https://github.com/lieff/minimp3) | MP3 decoding | CC0 1.0 | `third_party/minimp3_LICENSE.txt` |
| Atkinson Hyperlegible | built-in fallback and UI font | SIL OFL 1.1 | `assets/fonts/OFL-atkinsonhyperlegible.txt` |
| Fira Sans | interface font | SIL OFL 1.1 | `assets/fonts/OFL-firasans.txt` |
| Spectral | default text font | SIL OFL 1.1 | `assets/fonts/OFL-spectral.txt` |
| Courier Prime | text font | SIL OFL 1.1 | `assets/fonts/OFL-courierprime.txt` |
| IBM Plex Mono | monospaced font | SIL OFL 1.1 | `assets/fonts/OFL-ibmplexmono.txt` |
| Extra font pack (Literata, Lora, EB Garamond, PT Serif, Gentium Book Plus, IBM Plex Serif, iA Writer Quattro/Duo/Mono, Special Elite, Cutive Mono) | `FOLIO/FONTS/EXTRA` (included) | SIL OFL 1.1 / Apache 2.0 (Special Elite) | license file in each folder of `assets/fonts-extras/` |
| Noto Sans SC, JP, KR | `FOLIO/FONTS/_LANGUAGE` (included; downloaded at build time by `tools/fetch-cjk-fonts.ps1`) | SIL OFL 1.1 | `FOLIO/FONTS/_LANGUAGE/OFL-NotoSansCJK.txt` |

The SIL Open Font License allows the fonts to be bundled and redistributed with software;
the font files themselves remain under the OFL.
