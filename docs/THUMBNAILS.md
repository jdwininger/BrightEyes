# Thumbnails — Improvements 🧭

This project includes a small set of changes to improve thumbnail performance and correctness:

- ✅ Debounced thumbnail loading (short delay for images, longer delay for videos to avoid churn while scrolling)
- ✅ Bounded worker pool for expensive video-frame extraction to avoid thread explosion
- ✅ In-memory LRU cache keyed by path+mtime+size to avoid repeated decodes within a session
- ✅ Guarded binding/unbinding so recycled widgets don't get stale updates

These changes are implemented in `src/thumbnails.c` with a default in-memory cache size and cleanup on dispose. Run the app on a large directory to see smoother scrolling and fewer re-decodes.
