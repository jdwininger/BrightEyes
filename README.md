# BrightEyes

BrightEyes is a modern GTK4 image viewer written in C with libadwaita for GNOME integration. Features planned:

- Image viewing (zoom/pan planned)
- Directory browsing and thumbnails
- Slideshow with fade transitions
- Metadata viewing
- Video playback (via GStreamer gtksink)
- OCR via libtesseract

Build (development machine with necessary -dev packages):

meson setup build
meson compile -C build
./build/brighteyes

---

## 📚 Docs & GitHub Pages

Minimal documentation is available in the `docs/` directory and will be published to GitHub Pages when the repository is created and the workflow runs.

- `docs/` contains a short overview and a focused page about the thumbnail improvements (debounce, bounded video worker, in-memory LRU cache).
- A GitHub Actions workflow `.github/workflows/pages.yml` will publish `docs/` to GitHub Pages on pushes to `main`.

**Published site:** https://jdwininger.github.io/BrightEyes/  
*(may take a few minutes to appear after the first deploy)*

License: GPLv3

## 📦 Archive support (CBZ/CBR)

- CBZ (ZIP-based comic book) and CBR (RAR-based) support are available when building with libarchive present on the system (pkg-config name: `libarchive`).
- On many distributions install `libarchive` development package (e.g., `libarchive-dev` or `libarchive-devel`) and Meson will detect it automatically.
- If libarchive is unavailable the application will still build and run but archives will not be enumerated.

Note: RAR/CBR support depends on libarchive's available RAR support which can vary by platform and libarchive build configuration.

## Publishing

- GitHub Releases: RPM and DEB artifacts are attached by CI.

### Flatpak

There is a Flatpak manifest at `org.jeremy.BrightEyes.yml` at the repo root. I added CI to build a Flatpak bundle on push and on release and upload the bundle as an artifact.


