# Manual Test Plan: Zoom Functionality

## 1. Basic Zoom
- [ ] Open an image.
- [ ] Click "Zoom In" button.
  - Expect: Image grows larger. Viewport remains centered on the same content point.
- [ ] Click "Zoom Out" button.
  - Expect: Image shrinks. Viewport remains centered.

## 2. Fit Modes
- [ ] Cycle "Fit" button (Window -> Width -> 100%?).
  - Note: Current implementation toggles Fit Window / Fit Width.
- [ ] Select "Fit to Window".
  - [ ] Resize application window.
  - Expect: Image resizes to stay fully visible.
- [ ] Select "Fit to Width".
  - [ ] Resize application window horizontally.
  - Expect: Image width matches window width. Vertical scrolling enabled if image is tall.

## 3. Panning & Zoom
- [ ] Zoom in to 100% or 125%.
- [ ] Click and drag to pan to the bottom-right corner.
- [ ] Zoom In again.
  - Expect: Viewport stays centered on the detail you were looking at (bottom-right area). It should NOT jump to top-left.
- [ ] Zoom Out.
  - Expect: Viewport stays roughly in same area until limits reached.

## 4. Keyboard Shortcuts
- [ ] Ctrl + `+` : Zoom In.
- [ ] Ctrl + `-` : Zoom Out.
- [ ] Ctrl + `0` : Fit (Toggle).

## 5. Edge Cases
- [ ] Rotate image (if supported with zoom).
  - Expect: Zoom level maintained or reset cleanly.
- [ ] Open new image while zoomed in.
  - Expect: Reset to default fit mode (Fit to Window).
