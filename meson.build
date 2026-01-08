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
  'src/ocr.c'
)

executable('brighteyes', sources + resources,
  dependencies : [gtk_dep, adwaita_dep, gdkpixbuf_dep, gst_dep, gstvideo_dep, tesseract_dep, lept_dep],
  install : true
)

install_data('data/org.brightEyes.BrightEyes.desktop', install_dir : 'share/applications')
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/1024x1024/apps', rename : 'org.brightEyes.BrightEyes.png')
install_data('icon-1024.png', install_dir : 'share/icons/hicolor/512x512/apps', rename : 'org.brightEyes.BrightEyes.png')
install_data('LICENSE', install_dir : 'share/licenses/brighteyes')
install_data('README.md', install_dir : 'share/doc/brighteyes')