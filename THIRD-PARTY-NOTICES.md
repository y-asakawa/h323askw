# Third-Party Notices

Last reviewed: 2026-06-24

H323ASKW contains source derived from third-party projects and can be built or
packaged with additional third-party libraries and codec plugins. Those
components remain under their own licenses and are not relicensed by
H323ASKW's `LICENSE.md`.

The maintainer publishes only the H323ASKW source repository, not DMGs,
application bundles, or executables. This file is an attribution and license
index for the source repository and the third-party components used by its
local build and packaging process. It is not a substitute for the complete
license texts or source-code availability materials required if someone else
distributes binaries.

Potential, build-dependent, and release-specific dependencies are documented
separately in `docs/DEPENDENCY-LICENSE-REVIEW.md`. The exact contents and
licenses of any binary distribution must be checked against the completed
application bundle.

## Source Origin and Core Libraries

| Component | Use in H323ASKW | License | Project |
| --- | --- | --- | --- |
| CallGen323 | Original Code from which H323ASKW is derived | Mozilla Public License 1.0 | https://github.com/willamowius/callgen323 |
| H323Plus | H.323 protocol and media library | Mozilla Public License 1.0; individual files may contain additional or alternative notices | https://github.com/willamowius/h323plus |
| PTLib | Portable runtime and media-device library | Mozilla Public License 1.0; individual files may contain additional or alternative notices | https://github.com/willamowius/ptlib |
| H323Plus Plugins | Audio and video codec plugins | Mixed; see the plugin section below and the notices in each source file | https://www.h323plus.org/ |

The complete Mozilla Public License 1.0 text used by H323ASKW is in
`LICENSE.md`. Original copyright and license notices in source files must be
retained.

## Components Used or Potentially Bundled by the macOS Build

The `create_app_bundle.sh` packaging script copies or can copy the following
components into `H323ASKW.app`.

| Component | License or licensing option used for an open-source build | Project |
| --- | --- | --- |
| Qt 6 Core, Gui, Widgets, DBus and plugins | GNU LGPL 3.0, GNU GPL 3.0, or another license expressly offered for the specific Qt module or component; a valid commercial Qt license may also apply. Verify every bundled module and plugin | https://www.qt.io/licensing/ |
| FFmpeg libraries | Build-dependent: normally LGPL 2.1-or-later, but GPL applies when GPL components are enabled | https://ffmpeg.org/legal.html |
| x264 | GNU GPL 2.0-or-later, or a separately obtained commercial license | https://www.videolan.org/developers/x264.html |
| x265 | GNU GPL 2.0-or-later, or separately obtained alternative or commercial rights; verify the exact version and terms | https://www.x265.org/ |
| OpenSSL 3.x | Apache License 2.0 | https://openssl-library.org/source/license/ |
| PortAudio | MIT license | https://www.portaudio.com/ |
| SpeexDSP | BSD 3-Clause license | https://www.speex.org/ |
| Speex | BSD 3-Clause license | https://www.speex.org/ |
| mpg123 / libmpg123, when bundled | GNU LGPL 2.1-only according to Homebrew package metadata; verify the exact bundled build | https://www.mpg123.de/ |
| SDL2 compatibility library, when included | zlib license | https://github.com/libsdl-org/sdl2-compat |

Qt itself contains third-party code under additional licenses. The matching Qt
third-party notices for the exact Qt release must also accompany a binary
distribution:

https://doc.qt.io/qt-6/licenses-used-in-qt.html

### Reviewed Development FFmpeg Build

The FFmpeg build reviewed on 2026-06-24 was configured with GPL and version 3
components enabled, including `--enable-gpl`, `--enable-version3`,
`--enable-libx264`, and `--enable-libx265`.

That reviewed build reported its license as GNU GPL version 3 or later. This
information applies only to the reviewed development build and does not
determine the license of a later release build.

## Codec Plugin Notices

The H323Plus plugin tree contains mixed licensing and, for some codecs, patent
or royalty requirements.

| Plugin or codec | Relevant notices |
| --- | --- |
| H.264 | Plugin files include MPL 1.0 or dual MPL 1.0/GPL 2.0-or-later notices. GPL-only helper files and the GPL-licensed x264 library may also be used. H.264 patent rights are separate from copyright licensing. |
| H.263-FFmpeg | Plugin source is MPL 1.0 and links to FFmpeg. The effective FFmpeg license depends on the exact FFmpeg build. |
| H.261-vic | Includes MPL 1.0 code and legacy vic code with a BSD-style license containing an advertising acknowledgement requirement. Preserve all source notices. |
| G.722 | Includes MPL 1.0 wrapper code and codec files offered under GPL 2.0 or LGPL 2.1 terms. Check each included source file. |
| G.722.1 | Wrapper code permits copying, use, sale, distribution, and modification with preservation of its notice. The codec uses ITU-T reference code identified as being under the ITU-T General Public License (G.191). The source also identifies patent-related conditions. |
| G.722.2 / AMR-WB | The source identifies AMR-WB as patented and states that use requires a license from VoiceAge. Do not include or enable this plugin in a public binary release without separately confirming the current patent and royalty requirements. |

## Apple System Components

H323ASKW uses frameworks and libraries supplied with macOS, including system
audio, video, networking, and user-interface frameworks. Those components are
provided under Apple's applicable SDK and operating-system terms and are not
redistributed as third-party project files by this repository.

## Requirements If Redistributing Binaries

The maintainer does not publish binary distributions. This publication policy
does not limit rights granted by the applicable licenses. Anyone who chooses
to distribute a DMG, application bundle, installer, or other binary must first:

1. Inventory every executable, dynamic library, framework, and plugin in the
   final release artifact, including recursively copied Homebrew and Qt
   dependencies.
2. Include the complete applicable license texts, copyright notices,
   attribution notices, and other required notices for every bundled
   component, preferably under:
   `H323ASKW.app/Contents/Resources/licenses/`.
3. Satisfy the applicable source-code and relinking requirements for each
   bundled component. Depending on the license, component, linking method, and
   distribution method, this may require providing corresponding source code,
   a valid written offer, relinking materials, installation information, build
   scripts, configuration details, or locally applied patches.
4. Preserve all notices in MPL-covered source files and document modifications
   to the CallGen323 Original Code in `CHANGES.md`.
5. Record the exact version, source revision, build configuration, and license
   output of FFmpeg, Qt, x264, x265, H323Plus, PTLib, and each bundled plugin
   for every release.
6. Verify codec patent, royalty, trademark, cryptography, and export-control
   requirements separately. Open-source copyright licenses do not necessarily
   grant patent, trademark, or regulatory rights.

The reviewed development bundle combines MPL 1.0-covered H323ASKW code with
GPL-covered codec components. MPL 1.0 and GPL compatibility is a material
distribution issue, and this notice file alone does not resolve that issue.

Publishing source code is legally distinct from distributing a prebuilt
application bundle. The reviewed GPL-enabled development bundle has not been
cleared for redistribution. The applicable licensing model would have to be
resolved, for example by:

* using a compatible LGPL-only FFmpeg build;
* excluding GPL codec libraries and plugins;
* using a sufficiently separated external-process architecture where legally
  appropriate;
* obtaining the necessary commercial or alternative licenses; or
* obtaining qualified legal review.

No endorsement by any third-party project or contributor is implied.
