project('brighteyes', 'c',
  version : '0.1',
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
gstvideo_dep = dependency('gstreamer-video-1.0', required: false)

gnome = import('gnome')
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

executable('brighteyes', sources + resources,
  dependencies : deps,
  install : true
)

install_data('data/org.jeremy.BrightEyes.desktop', install_dir : 'share/applications')
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/1024x1024/apps', rename : 'org.jeremy.BrightEyes.png')
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/512x512/apps', rename : 'org.jeremy.BrightEyes.png')
install_data('LICENSE', install_dir : 'share/licenses/brighteyes')
install_data('README.md', install_dir : 'share/doc/brighteyes')