project('brighteyes', 'c',
  version : '0.2.0',
  meson_version : '>=0.56'
)

adwaita_dep = dependency('libadwaita-1', required : true)
gtk_dep = dependency('gtk4', required : true)
gdkpixbuf_dep = dependency('gdk-pixbuf-2.0', required: true)
# OCR (Tesseract + Leptonica)
# libtesseract provides the OCR engine; leptonica (pkg-config name: lept) is used to read various image
# formats directly from disk. On systems without these pkg-config files, adjust Flatpak manifest to bundle
# the runtime libraries.
tesseract_dep = dependency('tesseract', required : false)
lept_dep = dependency('lept', required : false)

# Optional archive support (libarchive) for CBZ/CBR
libarchive_dep = dependency('libarchive', required: false)

gst_dep = dependency('gstreamer-1.0', required: false)
glib_dep = dependency('glib-2.0', required: true)
gstvideo_dep = dependency('gstreamer-video-1.0', required: false)

gio_dep = dependency('gio-2.0', required: true)

gnome = import('gnome')
fs = import('fs')
resources = gnome.compile_resources('brighteyes-resources',
  'brighteyes.gresource.xml',
  source_dir : '.'
)

sources = files(
  'src/main.c',
  'src/viewer.c',
  'src/curator.c',
  'src/window.c',
  'src/thumbnails.c',
  'src/metadata.c',
  'src/ocr.c',
  'src/archive.c'
)

# If libarchive is available, expose a compile-time define to enable archive code
deps = [gtk_dep, adwaita_dep, gdkpixbuf_dep, gst_dep, gstvideo_dep, tesseract_dep, lept_dep]
if libarchive_dep.found()
  deps += libarchive_dep
  add_project_arguments('-DHAVE_LIBARCHIVE', language : 'c')
endif

# Ensure math library is linked for functions like exp()
add_project_link_arguments('-lm', language : 'c')

bex = executable('brighteyes', sources + resources,
  dependencies : deps,
  install : true
)

# In-process CLI integration smoke for the Save-frame flow. Fast, non-interactive
# and tolerant of missing GStreamer (falls back to image-only capture).
# This prevents regressions of the --test-save-frame path.
test('save-frame-cli', bex, args : ['--test-save-frame', 'test-out-save-frame.png'],
  workdir : meson.project_source_root())

# Small test tool to exercise archive conversion logic from the command line
executable('archive-convert-test', 'tools/convert_test.c',
  dependencies : [libarchive_dep, glib_dep],
  install : false
)

executable('zip-test', 'tools/zip_test.c',
  dependencies : libarchive_dep,
  install : false
)

if fs.exists('tools/image_convert_test.c')
  image_convert_test_exe = executable('image-convert-test', 'tools/image_convert_test.c',
    dependencies : [gdkpixbuf_dep, glib_dep],
    install : false
  )

    test('image-convert', image_convert_test_exe, args : ['icon-1024.png', 'test-out.png', 'test-out.jpg', '85', '#ffffff'],
      workdir : meson.project_source_root())
endif



executable('run-archive-convert', ['tools/run_archive_convert.c', 'src/archive.c'],
  dependencies : [glib_dep, libarchive_dep, gio_dep],
  install : false
)

executable('archive-convert-verify', ['tools/convert_verify.c', 'src/archive.c'],
  dependencies : [glib_dep, libarchive_dep, gio_dep],
  install : false
)

if libarchive_dep.found()
  th_exe = executable('brighteyes-thumbnailer', 'tools/thumbnailer.c',
    dependencies : [libarchive_dep, gdkpixbuf_dep, glib_dep],
    install : true
  )

  test_cbz = files('test.cbz')
  test('thumbnailer', th_exe, args : [test_cbz, 'test-thumb.png', '64'])

  # Smoke test: run a tiny GStreamer-based capture pipeline against an
  # image (single-frame source) to validate the same decode + sink code
  # used by the thumbnails capture path. Only add if GStreamer is
  # available.
  if gst_dep.found()
    video_capture_smoke = executable('video-frame-capture-smoke', 'tools/video_frame_capture_smoke.c',
      dependencies : [gst_dep, gdkpixbuf_dep, glib_dep],
      install : false
    )
    test('video-frame-capture-smoke', video_capture_smoke, workdir : meson.project_source_root())

    if fs.exists('tools/save_frame_gui_test.c')
      save_frame_gui_test = executable('save-frame-gui', ['tools/save_frame_gui_test.c', 'src/thumbnails.c', 'src/viewer.c'],
        dependencies : [gst_dep, gdkpixbuf_dep, glib_dep, gtk_dep, adwaita_dep],
        install : false
      )
      test('save-frame-gui', save_frame_gui_test, workdir : meson.project_source_root())
    endif
  endif

  # Exercise the video-frame capture pipeline using a single-image source
  # (GStreamer treats images as single-frame videos). This validates the
  # same capture code path used by the viewer's "Save frame" action.
    test('thumbnailer-video-frame', th_exe, args : ['icon-1024.png', 'test-out-video-frame.png', '64'],
      workdir : meson.project_source_root())

endif

install_data('data/org.jeremy.BrightEyes.desktop', install_dir : 'share/applications')
install_data('data/org.jeremy.BrightEyes.xml', install_dir : 'share/mime/packages')
install_data('data/org.jeremy.BrightEyes.thumbnailer', install_dir : 'share/thumbnailers')
if fs.exists('data/org.jeremy.BrightEyes.gschema.xml')
  install_data('data/org.jeremy.BrightEyes.gschema.xml', install_dir : 'share/glib-2.0/schemas')
endif
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/1024x1024/apps', rename : 'org.jeremy.BrightEyes.png')
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/512x512/apps', rename : 'org.jeremy.BrightEyes.png')
install_data('LICENSE', install_dir : 'share/licenses/brighteyes')
install_data('README.md', install_dir : 'share/doc/brighteyes')