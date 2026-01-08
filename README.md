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
