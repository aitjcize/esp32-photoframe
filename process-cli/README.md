# PhotoFrame CLI Image Processor

Node.js CLI tool that shares the exact same image processing code (`image-processor.js`) with the webapp for consistent results.

## Features

- **Shared Code**: Uses the same `image-processor.js` as the webapp - zero duplication
- **Identical Processing**: S-curve tone mapping, saturation, and dithering match webapp exactly
- **Measured Palette**: Dithers using actual e-paper colors for accurate color matching
- **Dual Output Modes**: Render with theoretical (bright) or measured (preview) palette
- **Portrait Rotation**: Automatically rotates portrait images 90° clockwise
- **Thumbnail Generation**: Creates thumbnails just like the web interface

## Installation

```bash
cd process-cli
npm install
```

**System Requirements:**
- Node.js 14+ up to Node.js 20 (dependencies are no longer supported for higher versions)
- macOS/Linux: Install Cairo dependencies (see below)

**macOS:**
```bash
brew install pkg-config cairo pango libpng jpeg giflib librsvg pixman
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install build-essential libcairo2-dev libpango1.0-dev libjpeg-dev libgif-dev librsvg2-dev
```

## Usage

Basic usage:
```bash
node cli.js input.jpg
```

Specify output directory:
```bash
node cli.js input.jpg -o /path/to/output
```

Custom S-curve and saturation:
```bash
node cli.js input.jpg --scurve-strength 0.8 --saturation 1.5
```

Preview mode (render with measured palette):
```bash
node cli.js input.jpg --render-measured -o preview/
```

## Options

```
Usage: photoframe-process [options] <input>

Arguments:
  input                       Input image file (JPEG/PNG)

Options:
  -V, --version               output the version number
  -o, --output-dir <dir>      Output directory (default: ".")
  --suffix <suffix>           Suffix to add to output filename (default: "")
  --no-thumbnail              Skip thumbnail generation
  --scurve-strength <value>   S-curve overall strength (0.0-1.0) (default: 0.9)
  --scurve-shadow <value>     S-curve shadow boost (0.0-1.0) (default: 0.0)
  --scurve-highlight <value>  S-curve highlight compress (0.5-3.0) (default: 1.7)
  --scurve-midpoint <value>   S-curve midpoint (0.3-0.7) (default: 0.5)
  --saturation <value>        Saturation multiplier (1.0=normal, >1.0=more vibrant) (default: 1.2)
  --render-measured           Render BMP with measured palette colors (darker output for preview)
  --processing-mode <mode>    Processing algorithm: enhanced (default, with S-curve) or stock (Waveshare original) (default: "enhanced")
  -h, --help                  display help for command
```

## Output Files

For input `photo.jpg`, the tool generates:
- `photo.bmp`: Processed BMP for e-paper display (800x480, dithered, 7-color)
  - Default: Theoretical palette (bright colors for ESP32)
  - `--render-measured`: Measured palette (darker, matches actual e-paper)
- `photo.jpg`: Thumbnail for web interface (160x96)

## Processing Pipeline

1. **Load Image**: Read JPEG/PNG using node-canvas
2. **Rotate**: If portrait, rotate 90° clockwise
3. **Resize**: Cover mode scaling to 800x480 (scale and crop to fill)
4. **Saturation**: Apply HSL-based saturation adjustment (default: 1.2x)
5. **Image Processing** (mode-dependent):
   - **Enhanced Mode** (default): S-curve tone mapping + saturation + measured palette dithering
     - Strength: Overall curve intensity (default: 0.9)
     - Shadow Boost: Brighten shadows (default: 0.0)
     - Highlight Compress: Protect highlights (default: 1.7)
     - Midpoint: Shadow/highlight split (default: 0.5)
     - Saturation: Color vibrancy (default: 1.2)
   - **Stock Mode**: Simple Floyd-Steinberg dithering with theoretical palette (like original convert.py)
6. **Floyd-Steinberg Dithering**: Convert to 7-color palette
7. **BMP Output**: Write 800x480 indexed BMP
   - Default: Theoretical palette (for ESP32 upload)
   - `--render-measured`: Measured palette (for preview)
8. **Thumbnail**: Generate 160x96 JPEG thumbnail

## Examples

**Process with default settings:**
```bash
node cli.js vacation.jpg -o ~/photoframe-images/
```

**Stronger S-curve for flat images:**
```bash
node cli.js photo.jpg --scurve-strength 1.0 --scurve-highlight 3.0 -o output/
```

**More vibrant colors:**
```bash
node cli.js photo.jpg --saturation 1.5 -o output/
```

**Preview mode (see actual e-paper appearance):**
```bash
node cli.js photo.jpg --render-measured -o preview/
```

**Stock Waveshare algorithm (no S-curve, like original convert.py):**
```bash
node cli.js photo.jpg --processing-mode stock -o output/
```

**Batch process multiple images:**
```bash
for img in *.jpg; do
    node cli.js "$img" -o processed/
done
```

**Custom parameters with suffix:**
```bash
node cli.js photo.jpg --scurve-strength 0.8 --saturation 1.3 --suffix "_s08_sat13" -o output/
# Outputs: photo_s08_sat13.bmp, photo_s08_sat13.jpg
```

## Default Parameters

The CLI uses the same defaults as the webapp:
- **Processing Mode**: Enhanced (with S-curve tone mapping)
- **S-Curve Strength**: 0.9 (strong tone mapping)
- **Shadow Boost**: 0.0 (neutral shadows)
- **Highlight Compress**: 1.5 (moderate highlight protection)
- **Midpoint**: 0.5 (balanced shadow/highlight split)
- **Saturation**: 1.5 (moderate saturation)
- **Color Method**: RGB (simple Euclidean distance)

## Output Modes

**Default (Theoretical Palette):**
- Bright, vibrant colors
- For uploading to ESP32
- ESP32 firmware will display with measured palette

**Preview Mode (`--render-measured`):**
- Darker, muted colors
- Shows actual e-paper appearance
- For testing and preview purposes
