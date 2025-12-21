# PhotoFrame CLI Image Processor

Desktop CLI tool that replicates the exact same image processing pipeline as the ESP32 PhotoFrame firmware.

## Features

- **Identical Processing**: Uses the same algorithms as the ESP32 firmware
- **Same Pipeline**: Contrast → Brightness (f-stop) → Floyd-Steinberg dithering
- **7-Color Palette**: Matches the e-paper display palette exactly
- **Portrait Rotation**: Automatically rotates portrait images 90° clockwise
- **Thumbnail Generation**: Creates thumbnails just like the web interface

## Installation

```bash
cd photoframe-cli
pip install -r requirements.txt
chmod +x photoframe_process.py
```

## Usage

Basic usage:
```bash
./photoframe_process.py input.jpg
```

Specify output directory:
```bash
./photoframe_process.py input.jpg -o /path/to/output
```

Custom brightness and contrast:
```bash
./photoframe_process.py input.jpg -b 0.5 -c 1.5
```

## Options

- `-o, --output-dir DIR`: Output directory (default: current directory)
- `-b, --brightness FLOAT`: Brightness f-stop adjustment (default: 0.3)
- `-c, --contrast FLOAT`: Contrast multiplier (default: 1.3)

## Output Files

For input `photo.jpg`, the tool generates:
- `photo.bmp`: Processed BMP for e-paper display (800x480, dithered, 7-color)
- `photo.jpg`: Thumbnail for web interface (480x800 max)

## Processing Pipeline

1. **Load JPEG**: Read input image
2. **Rotate**: If portrait, rotate 90° clockwise
3. **Resize**: Fit to 800x480 display (maintain aspect ratio, white padding)
4. **Contrast**: Apply contrast adjustment `((pixel - 128) * contrast) + 128`
5. **Brightness**: Apply f-stop adjustment `pixel * 2^fstop`
6. **Dither**: Floyd-Steinberg dithering to 7-color palette
7. **Save BMP**: Write processed image
8. **Save Thumbnail**: Generate thumbnail from original

## Examples

Process with default settings (0.3 f-stop, 1.3 contrast):
```bash
./photoframe_process.py vacation.jpg -o ~/photoframe-images/
```

Higher contrast for washed-out images:
```bash
./photoframe_process.py photo.jpg -c 1.5 -o output/
```

Batch process multiple images:
```bash
for img in *.jpg; do
    ./photoframe_process.py "$img" -o processed/
done
```

## Notes

- The tool uses the same default values as the ESP32 firmware (brightness: 0.0, contrast: 1.1)
- Portrait images are automatically rotated to landscape orientation
- The 7-color palette matches the e-paper display exactly
- Floyd-Steinberg dithering uses the same algorithm as the firmware
