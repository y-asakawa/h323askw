# Change Log

This file records H323ASKW modifications to CallGen323 and their dates under
Section 3.3 of the Mozilla Public License Version 1.0. H323ASKW is derived
from CallGen323; the Initial Developer of the Original Code is Benny L.
Prijono. Commit dates below come from the retained Git history.
The Git log contains the individual commit timestamps and changes.

The first retained source commit is dated 2025-12-11. It already contains
earlier H323ASKW work, including the macOS Apple Silicon port and Qt 6 client.
The exact dates of changes made before that commit, including work described
as beginning in 2024, cannot be verified from this repository history.

## 2026

* 2026-09-24: Improved macOS build checks and App Bundle dependency validation.
* 2026-06-22 to 2026-06-24: Added security and build automation, corrected
  license documentation, and prepared public-source notices and guides.
* 2026-03-02: Fixed multi-device audio and video selection at runtime.
* 2026-02-27: Added camera overlay text, Vision-based background blur, and
  content-only recording; improved video state on disconnect.
* 2026-02-18: Added audio quality profiles and runtime SpeexDSP controls.
* 2026-02-09 to 2026-02-12: Added multi-camera mosaic and recording controls;
  improved Qt device handling, bundle packaging, and audio reliability.
* 2026-02-02: Added SpeexDSP processing and macOS permission handling.
* 2026-01-30: Added microphone gain compensation and audio gain controls.
* 2026-01-05 to 2026-01-14: Extended H.239 sharing, capture, reception, local
  preview, and bandwidth behavior.

## 2025

* 2025-12-24: Added H.239 content display.
* 2025-12-11: Imported the first retained H323ASKW source version, already
  adapted from CallGen323 into a macOS Apple Silicon H.323 client with a Qt 6
  interface, media device handling, codec support, and an application bundle.
