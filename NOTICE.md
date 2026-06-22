# Notices

H323ASKW is derived from the H.323 call generator project:

- Upstream project: https://github.com/willamowius/callgen323
- Upstream author/maintainer: Jan Willamowius and contributors
- Original project purpose: H.323 call generator for load testing

H323ASKW has been modified into a macOS Apple Silicon H.323 video client with
Qt6 UI integration, app bundle packaging, local media device handling, recording,
and related application features. A summary of modifications and dates is kept
in `CHANGES.md`.

The upstream callgen323 source files identify their license as Mozilla Public
License Version 1.0. H323ASKW keeps the Mozilla Public License 1.0 text in
`LICENSE.md`. Source files derived from the upstream project retain their
original copyright notices where present.

H323ASKW also depends on external projects including:

- PTLib: https://github.com/willamowius/ptlib
- H323Plus: https://github.com/willamowius/h323plus
- Qt: https://www.qt.io/
- FFmpeg: https://ffmpeg.org/
- x264: https://www.videolan.org/developers/x264.html
- SpeexDSP: https://www.speex.org/
- Platform frameworks provided by macOS

Those dependencies remain under their respective licenses. Source publication of
H323ASKW does not include those third-party libraries. If distributing a binary
or app bundle that includes third-party dynamic libraries or plugins, include
the corresponding third-party license notices and comply with the distribution
terms of each bundled component.
