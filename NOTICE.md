## Notices

H323ASKW

H323ASKW is derived from the CallGen323 H.323 call generator project.

* Upstream project: https://github.com/willamowius/callgen323
* Original Code: CallGen323
* Initial Developer: Benny L. Prijono
* Upstream maintainer: Jan Willamowius
* Upstream contributors: Equivalence Pty. Ltd. and other contributors
* H323ASKW contributor: Y. Asakawa (2024-2026) - Qt 6 video client,
  macOS Apple Silicon port, and application extensions
* Original purpose: H.323 call generation and load testing

Portions created by Jan Willamowius are Copyright (C) 2008-2018
Jan Willamowius. All Rights Reserved.

Portions created by Yoshifumi Asakawa are Copyright (C) 2024-2026
Yoshifumi Asakawa. All Rights Reserved.

H323ASKW has been modified and extended into a macOS Apple Silicon H.323
audio and video client. The modifications include Qt 6 user-interface
integration, macOS application bundle packaging, local audio and video device
handling, call recording, and related application features.

A summary of the modifications and their dates is provided in `CHANGES.md`.

The CallGen323 source files are licensed under the Mozilla Public License
Version 1.0. Files derived from or containing CallGen323 source code remain
subject to the Mozilla Public License Version 1.0 and retain the original
copyright and license notices where present.

The `version.h` header in CallGen323 also carries the original Portable
Windows Library notice and credits Equivalence Pty. Ltd. as its Initial
Developer. That file-specific notice is retained in H323ASKW's `version.h`.

Unless otherwise noted, H323ASKW-authored source files, build scripts, and
application metadata are also distributed under Mozilla Public License
Version 1.0. File-level notices identify their copyright holders.

The complete Mozilla Public License Version 1.0 text is included in
[`LICENSE.md`](LICENSE.md).

Copyright notices for H323ASKW modifications are included in the corresponding
source files.

## Source Code Availability

The source code for H323ASKW, including modifications to the Covered Code, is
available from the H323ASKW GitHub repository under the terms of the Mozilla
Public License Version 1.0.

The maintainer publishes source code only and does not provide DMGs,
application bundles, or other prebuilt binaries. This publication policy does
not limit rights granted to others under the applicable licenses.

## Third-Party Software

H323ASKW uses or may be built with the following external projects:

* PTLib: https://github.com/willamowius/ptlib
* H323Plus: https://github.com/willamowius/h323plus
* Qt: https://www.qt.io/
* FFmpeg: https://ffmpeg.org/
* x264: https://www.videolan.org/developers/x264.html
* x265: https://www.x265.org/
* OpenSSL: https://openssl-library.org/
* PortAudio: https://www.portaudio.com/
* SpeexDSP: https://www.speex.org/
* Frameworks and libraries provided by macOS

These projects remain subject to their respective licenses. Publication of the
H323ASKW source code does not, by itself, include or relicense these third-party
projects.

Third-party libraries and codec plugins are documented in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). Build-specific dependency
and license review information is maintained in
[`docs/DEPENDENCY-LICENSE-REVIEW.md`](docs/DEPENDENCY-LICENSE-REVIEW.md).

Binary releases, macOS application bundles, installers, and other packaged
distributions that include third-party libraries, frameworks, plugins, or
executables must include the applicable copyright notices, license texts, and
source-code offers or source-code availability information required by those
third-party licenses.

No endorsement by the original CallGen323 developers, contributors, or
third-party software projects is implied.
