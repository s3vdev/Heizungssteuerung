# Release v2.2.8

## 🎨 UI Improvements

### Dark Mode Enhancements
- ✅ **Comprehensive dark mode support** - All UI elements now properly styled for dark theme
- ✅ **Improved readability** - Fixed many text colors and backgrounds that were unreadable in dark mode
- ✅ **Consistent styling** - All cards, buttons, inputs, and labels now use CSS variables for proper dark mode support
- ✅ **Better contrast** - Enhanced color scheme for better visibility in dark mode

### Checkbox Styling
- ✅ **Custom checkbox design** - Modern, styled checkboxes with smooth animations
- ✅ **Dark mode optimized** - Checkboxes are clearly visible in both light and dark themes
- ✅ **Hover effects** - Improved user feedback with hover states
- ✅ **Accessibility** - Better focus states for keyboard navigation

### CSS/JS File Serving
- ✅ **Fixed static file serving** - CSS and JavaScript files are now properly served from LittleFS
- ✅ **Added explicit routes** - Routes for `/assets/css/style.css` and `/assets/js/script.js`
- ✅ **Improved 404 handler** - Better handling of static files with proper content types

## 📦 Technical Changes

- Updated CSS variables for dark mode compatibility
- Added `btn-secondary` class styling
- Improved checkbox control styling in `style.css`
- Enhanced `onNotFound` handler in `main.cpp` for static file serving
- Added explicit routes for CSS and JS assets

## 🔧 Files Changed

- `data/index.html` - Dark mode improvements throughout
- `data/assets/css/style.css` - Complete dark mode overhaul and checkbox styling
- `src/main.cpp` - Added static file serving routes
- Version updated to v2.2.8

## 📥 Installation

1. Update firmware: Upload `.pio/build/esp32dev/firmware.bin` via OTA or USB
2. Update frontend: Upload `.pio/build/esp32dev/littlefs.bin` via OTA or USB

---

**Full Changelog**: https://github.com/s3vdev/Heizungssteuerung/compare/v2.2.7...v2.2.8

