# Change Log

This file documents H323ASKW modifications to the CallGen323 original code for
Mozilla Public License 1.0 section 3.3.

## 2024-2025

- Derived the project from CallGen323 and adapted it from a call generator into
  a macOS Apple Silicon H.323 video client.
- Added Qt6 user interface components for video display, call controls, media
  device selection, audio levels, and local preview handling.
- Added macOS app bundle packaging support and runtime dependency rewriting for
  PTLib, H323Plus, Qt, codec plugins, and related dynamic libraries.
- Added macOS permission bootstrap code for camera, microphone, local network,
  and firewall-related startup behavior.
- Added multi-device audio selection, gain controls, SpeexDSP integration, and
  recording support.
- Added video handling features including USB camera selection, H.239 content
  sharing, local/remote view handling, mosaic/overlay experiments, and Vision
  framework based person-mask support.
- Added H.235/H.460-related option handling and compatibility adjustments for
  H323Plus/PTLib behavior used by this application.
- Added repository metadata for public maintenance, including security policy,
  Dependabot configuration, CodeQL workflow, and dependency notices.

