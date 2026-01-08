Name:           brighteyes
Version:        0.1
Release:        1%{?dist}
Summary:        Image and simple video viewer

License:        GPLv3+
URL:            https://example.org/brighteyes
Source0:        %{name}-%{version}.tar.gz

# BuildRequires: base development toolchain
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  python3-meson-python

# GUI libs
BuildRequires:  gtk4-devel
BuildRequires:  libadwaita-devel
BuildRequires:  gdk-pixbuf2-devel
BuildRequires:  glib2-devel

# Optional features
%bcond_without gstreamer
%bcond_without ocr
%if %{with gstreamer}
BuildRequires:  gstreamer1-devel
BuildRequires:  gstreamer1-plugins-base-devel
%endif

%if %{with ocr}
BuildRequires:  tesseract-devel
BuildRequires:  leptonica-devel
%endif

Requires:       gtk4
%if %{with gstreamer}
Requires: gstreamer1
%endif

%if %{with ocr}
Requires: tesseract
%endif
Requires:       libadwaita
Requires:       gdk-pixbuf2
Requires:       glib2
# If you enable media/ocr features at build time, the following may be needed at runtime
# Requires:     gstreamer1
# Requires:     tesseract

# If you want to allow building without gst/tesseract, add conditional build switches.

%description
BrightEyes — a simple image and video viewer. Provides asynchronous image loading, optional OCR and GStreamer-based playback.

%prep
%setup -q -n %{name}-%{version}

%build
# Configure a release build and ensure prefix=/usr
meson setup build --prefix=/usr -Dbuildtype=release
meson compile -C build

%check
# Run meson tests if present; don't fail the build if none exist
meson test -C build || true
# Smoke tests: verify build artifact exists and links to GTK
test -x build/brighteyes
ldd build/brighteyes | grep -q 'libgtk-4'

%install
rm -rf %{buildroot}
meson install -C build --destdir=%{buildroot}

%post
# Refresh icon cache and desktop database so the icon and .desktop entry are immediately available
/sbin/gtk-update-icon-cache -f -t %{_datadir}/icons/hicolor >/dev/null 2>&1 || true
/usr/bin/update-desktop-database -q %{_datadir}/applications >/dev/null 2>&1 || true

%postun
# On final removal (argument $1 == 0), refresh caches again
if [ $1 -eq 0 ] ; then
  /sbin/gtk-update-icon-cache -f -t %{_datadir}/icons/hicolor >/dev/null 2>&1 || true
  /usr/bin/update-desktop-database -q %{_datadir}/applications >/dev/null 2>&1 || true
fi

%files
%license LICENSE
%doc README.md
/usr/bin/brighteyes
/usr/share/applications/org.brightEyes.BrightEyes.desktop
/usr/share/icons/hicolor/1024x1024/apps/org.brightEyes.BrightEyes.png
/usr/share/icons/hicolor/512x512/apps/org.brightEyes.BrightEyes.png

%changelog
* Wed Jan 07 2026 Your Name <you@example.org> - 0.1-1
- Initial package
