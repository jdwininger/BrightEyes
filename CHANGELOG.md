# Changelog

All notable changes to this project will be documented in this file.

## [0.2.2] - 2026-07-01
### Changed
- Reverted the aggressive OCR filtering heuristics that reduced full-image OCR quality.
- Added a warning for full-image OCR to steer mixed text/graphics images toward Selection OCR.
- Added a dated handoff document with the changes and general coding notes.

### Misc
- Version bumped to **0.2.2**.

## [0.2.0] - 2026-01-13 🚀
### Added
- Add `adwaita-icon-theme` to DEB packaging `fpm --depends` to ensure Adwaita icons are available on Ubuntu/Debian systems. 🛠️
- Update documentation (README, docs/) and add this `CHANGELOG.md`. ✍️

### Packaging
- Bump project and spec version to **0.2.0**. 📦

### Misc
- Minor packaging and docs cleanups.

## [0.1] - 2026-01-07
- Initial package
