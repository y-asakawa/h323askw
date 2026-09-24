# Dependency License Review

Last reviewed: 2026-06-24

This document records the license review of local H323ASKW builds and optional
binary packaging. The maintainer publishes source code only; no DMG, App
Bundle, or executable is offered. Potential and build-dependent dependencies
listed here are not necessarily present in every local build.

The public attribution index is `THIRD-PARTY-NOTICES.md`. This record does not
approve binary redistribution. Anyone considering it must replace assumptions
here with an inventory of their completed application bundle.

## Reviewed Source Revisions

| Component | Source repository | Revision reviewed |
| --- | --- | --- |
| H323Plus | https://github.com/willamowius/h323plus | `ea2072978f0334583b550dbfc8b5f6eb7303cbef` |
| PTLib | https://github.com/willamowius/ptlib | `f85a10209ac25fdd8bbb8e835c5da161d682e5ac` |
| H323Plus Plugins | https://github.com/y-asakawa/h323plus-plugins | `d7e2eae3d436b5290f0f3a34009f892147386091` |

These revisions identify the local source trees reviewed on the date above.
They are not automatically the revisions used by a later release.

The H323Plus Plugins repository was private when reviewed and is planned for
separate publication. Its link may be unavailable until then. A distributor
of a build containing those plugins must identify the exact source revision
and satisfy the applicable license obligations.

The bundle script now defaults to the public H323Plus plugin sources instead.
The private plugin revision above is historical review evidence, not a required
build dependency. Review the exact plugin sources and binary selected for each
release; this record does not establish equivalence between the public and
private plugin builds.

## Reviewed Local FFmpeg Build

The Homebrew FFmpeg build reviewed on the development machine reported:

```text
Version reviewed: N-124260-g9172ab1245
License reported by ffmpeg -L: GPL version 3 or later
```

Relevant configuration flags included:

```text
--enable-gpl
--enable-version3
--enable-libx264
--enable-libx265
```

FFmpeg is normally available under LGPL 2.1-or-later, but enabling GPL
components changes the effective license of that build. The result above is
specific to the reviewed binary. Run and preserve the output of
`ffmpeg -version` and `ffmpeg -L` for every release build.

No commercial x264 or x265 license has been assumed in this review.

## Potential FFmpeg and Homebrew Dependency Libraries

`create_app_bundle.sh` attempts to copy the following libraries when they are
available in the Homebrew installation. A library belongs in the final release
notices only when it is actually included in, linked by, or otherwise
distributed with the release artifact.

| Component | License indicated by reviewed package metadata |
| --- | --- |
| AOM | BSD 2-Clause |
| aribb24 | LGPL 3.0-only |
| Brotli | MIT |
| dav1d | BSD 2-Clause |
| Highway | Apache 2.0 or BSD 3-Clause |
| libjpeg-turbo | IJG, zlib, and BSD 3-Clause |
| JPEG XL | BSD 3-Clause |
| LAME | LGPL 2.0-or-later |
| mpg123 / libmpg123 | LGPL 2.1-only (Homebrew formula metadata checked 2026-09-24; verify the bundled build) |
| libogg | BSD 3-Clause |
| libpng | libpng 2.0 |
| libsoxr | LGPL 2.1-or-later |
| libvmaf | BSD 2-Clause with patent grant |
| libvorbis | BSD 3-Clause |
| libvpx | BSD 3-Clause |
| X11 libraries (`libX11`, `libXau`, `libxcb`, `libXdmcp`) | MIT |
| Little CMS | MIT |
| OpenCORE AMR | Apache 2.0 |
| OpenJPEG | BSD 2-Clause |
| Opus | BSD 3-Clause |
| rav1e | BSD 2-Clause |
| Snappy | BSD 3-Clause |
| Speex | BSD 3-Clause |
| SVT-AV1 | BSD 3-Clause |
| Theora | BSD 3-Clause |
| WebP | BSD 3-Clause |
| x264 | GPL 2.0-or-later or separately obtained commercial license |
| x265 | GPL 2.0-or-later or separately obtained alternative license |
| XZ / liblzma | 0BSD and GPL 2.0-or-later, depending on included files |

The packaging script also recursively copies Homebrew libraries referenced by
Qt. Depending on the installed Qt build, this can add ICU, GLib, PCRE2,
double-conversion, zstd, or other libraries. Development-machine presence does
not establish that a component was distributed.

## Codec Plugin Review

The H323Plus plugin source tree contains mixed license and patent notices.

| Plugin or codec | Review result |
| --- | --- |
| H.264 | Contains MPL 1.0 or dual MPL 1.0/GPL 2.0-or-later files, GPL-only helper files, and can use GPL-licensed x264. H.264 patent rights are separate. |
| H.263-FFmpeg | Plugin source is MPL 1.0 and links to FFmpeg. Its distribution analysis depends on the exact FFmpeg build. |
| H.261-vic | Contains MPL 1.0 code and legacy vic code with a BSD-style license that includes an advertising acknowledgement requirement. |
| G.722 | Contains MPL 1.0 wrapper code and codec files offered under GPL 2.0 or LGPL 2.1 terms. Review each included file. |
| G.722.1 | Contains a permissive wrapper notice and ITU-T reference code identified as using the ITU-T General Public License (G.191). Source notices also identify patent-related conditions. |
| G.722.2 / AMR-WB | Source states that AMR-WB is patented and use requires a license from VoiceAge. Exclude this plugin from public binaries unless current rights are separately confirmed. |

## Inventory Procedure If Redistributing Binaries

For any candidate `H323ASKW.app` intended for redistribution:

1. List all regular files and symbolic links under `Contents/Frameworks`,
   `Contents/PlugIns`, and `Contents/Resources/plugins`.
2. Run `otool -L` on the main executable, every dynamic library, every Qt
   framework binary, and every plugin.
3. Resolve recursive dependencies and identify the source package and exact
   version for each non-system library.
4. Record Qt module and plugin licenses using the notices shipped with the
   exact Qt release.
5. Record `ffmpeg -version`, `ffmpeg -L`, and the build configuration.
6. Compare the resulting inventory with `THIRD-PARTY-NOTICES.md` and include
   all required full license texts in the application bundle.
7. Archive the inventory, source revisions, local patches, build scripts, and
   source-code availability materials with the distribution record.

## Distribution Decision

The reviewed development configuration uses a GPL 3-or-later FFmpeg build and
GPL-covered codec components with MPL 1.0-covered H323ASKW code. A notice file
does not resolve the resulting license-compatibility issue.

The maintainer publishes source code only. This policy does not limit rights
granted to others by the applicable licenses. The reviewed configuration has
not been cleared for binary redistribution; anyone considering it must resolve
the licensing model through a compatible build, component exclusion,
appropriate architectural separation, separately obtained licenses, or
qualified legal review.

## Optional Binary Redistribution Review Template

This template is not an approval or an active maintainer release plan. Complete
it separately for any proposed binary distribution.

| Item | Recorded value |
| --- | --- |
| H323ASKW version | |
| Git commit | |
| Build date | |
| macOS build host version | |
| Apple clang version | |
| Qt version | |
| FFmpeg version and commit | |
| FFmpeg license output | |
| FFmpeg configure flags | |
| H323Plus revision | |
| PTLib revision | |
| Plugin revision | |
| x264 version or commit | |
| x265 version or commit | |
| Bundle inventory file | |
| License inventory file | |
| Corresponding source location | |
| Reviewer | |
| Review date | |

### Release Decision

* [ ] Source-only publication approved
* [ ] Binary distribution approved
* [ ] Binary distribution blocked pending license resolution
* [ ] Full license texts included
* [ ] Corresponding source materials archived
* [ ] GPL components excluded or separately resolved
* [ ] LGPL relinking requirements reviewed
* [ ] Qt plugins and third-party notices reviewed
* [ ] Codec patent and royalty review completed or escalated

## Primary License References

* GNU GPL 3.0:
  https://www.gnu.org/licenses/gpl-3.0.html
* Qt licensing:
  https://doc.qt.io/qt-6/licensing.html
* Qt third-party code:
  https://doc.qt.io/qt-6/licenses-used-in-qt.html
* FFmpeg licensing:
  https://ffmpeg.org/legal.html
* x264 licensing:
  https://www.videolan.org/developers/x264.html
* x265 licensing:
  https://www.x265.org/
* OpenSSL licensing:
  https://openssl-library.org/source/license/
