# Thumbnails — Improvements 🧭

*Included in release **v0.2.0* 🚀*

This project includes a small set of changes to improve thumbnail performance and correctness:

- ✅ Debounced thumbnail loading (short delay for images, longer delay for videos to avoid churn while scrolling)
- ✅ Bounded worker pool for expensive video-frame extraction to avoid thread explosion
- ✅ In-memory LRU cache keyed by path+mtime+size to avoid repeated decodes within a session
- ✅ Guarded binding/unbinding so recycled widgets don't get stale updates

These changes are implemented in `src/thumbnails.c` with a default in-memory cache size and cleanup on dispose. Run the app on a large directory to see smoother scrolling and fewer re-decodes.

## System thumbnails and packaging (CBZ/CBR)

BrightEyes ships a small thumbnailer helper and MIME file so file managers (GNOME Files, etc.) can show the first image from a CBZ/CBR archive.

Packaging notes:
- Install `data/org.jeremy.BrightEyes.xml` to `share/mime/packages/` and run `update-mime-database /usr/share/mime` during package postinstall.
- Install `data/org.jeremy.BrightEyes.thumbnailer` to `share/thumbnailers/` and install the `brighteyes-thumbnailer` helper to `bin/`.
- After installation run `update-desktop-database` and (optionally) clear thumbnail caches: `rm -rf ~/.cache/thumbnails/*` and restart the file manager.

Testing the helper locally:

```sh
# Generate 128px thumbnail for a local file (file path or file:// URI)
brighteyes-thumbnailer /path/to/comic.cbz /tmp/thumb.png 128
# Or point the thumbnailer directly at a URI (thumbnailers provide URIs):
brighteyes-thumbnailer file:///path/to/comic.cbz /tmp/thumb.png 128
```

If the thumbnailer is present and `org.jeremy.BrightEyes.xml` is installed, GNOME Files should show the first image as the thumbnail for CBZ/CBR files.

