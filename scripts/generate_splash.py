#!/usr/bin/env python3
"""Generate OOBE splash screen EPDGZ files for embedding in firmware.

Creates SVG splash screens by display resolution (not per-board), converts
to PNG via rsvg-convert, then uses process-cli to dither and produce EPDGZ.

Currently generates two variants:
  - 800x480   (landscape: Waveshare PhotoPainter 7.3", reTerminal E1002, XIAO EE04)
  - 1200x1600 (portrait: XIAO EE02)

Dependencies:
  pip install qrcode
  rsvg-convert (librsvg2-bin on Ubuntu, librsvg on macOS)
  node + process-cli (npm ci in process-cli/)
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

try:
    import qrcode
except ImportError:
    print("Install qrcode: pip install qrcode")
    sys.exit(1)

# App download URL
APP_URL = "https://aitjcize.github.io/esp32-photoframe/#app"

# QR code module size in SVG units
QR_MODULE_SIZE = 4

# Screen size variants to generate
SCREEN_SIZES = {
    "800x480": (800, 480),
    "1200x1600": (1200, 1600),
}

# Layout parameters per orientation (all values are fractions of width or height)
LAYOUT = {
    "landscape": {
        "icon_y": 0.25,  # icon vertical center (fraction of height)
        "icon_size": 0.28,  # icon size (fraction of min(width, height))
        "title_size": 0.06,  # title font size (fraction of height)
        "subtitle_size": 0.035,  # subtitle font size (fraction of height)
        "qr_y": 0.62,  # QR code top edge (fraction of height)
        "qr_size": 0.15,  # QR code size (fraction of width, capped by height)
        "qr_spacing": 0.30,  # distance between QR centers (fraction of width)
    },
    "portrait": {
        "icon_y": 0.30,  # icon vertical center (fraction of height)
        "icon_size": 0.25,  # icon size (fraction of min(width, height))
        "title_size": 0.035,  # title font size (fraction of height)
        "subtitle_size": 0.016,  # subtitle font size (fraction of height)
        "qr_y": 0.55,  # QR code top edge (fraction of height)
        "qr_size": 0.15,  # QR code size (fraction of width, capped by height)
        "qr_spacing": 0.20,  # gap between QR codes (fraction of width)
    },
}

# Path to process-cli
PROCESS_CLI = os.path.join(os.path.dirname(__file__), "..", "process-cli", "cli.js")


def generate_qr_svg_path(data: str, module_size: int = QR_MODULE_SIZE) -> tuple:
    """Generate QR code as SVG path data. Returns (path_d, qr_pixel_size)."""
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=0,
    )
    qr.add_data(data)
    qr.make(fit=True)
    matrix = qr.get_matrix()
    size = len(matrix)

    path_parts = []
    for y, row in enumerate(matrix):
        for x, cell in enumerate(row):
            if cell:
                px = x * module_size
                py = y * module_size
                path_parts.append(
                    f"M{px},{py}h{module_size}v{module_size}h-{module_size}z"
                )

    return " ".join(path_parts), size * module_size


def generate_splash_svg(width: int, height: int) -> tuple:
    """Generate splash screen SVG. Returns (svg_content, wifi_qr_x, wifi_qr_y, wifi_qr_size)."""
    is_landscape = width > height

    # Generate app QR code
    app_qr_path, app_qr_size = generate_qr_svg_path(APP_URL)

    L = LAYOUT["landscape" if is_landscape else "portrait"]

    icon_scale = min(width, height) * L["icon_size"]
    icon_x = width / 2
    icon_y = height * L["icon_y"]

    qr_scale = min(width * L["qr_size"], height * L["qr_size"]) / app_qr_size
    qr_y = height * L["qr_y"]

    if is_landscape:
        # QR codes positioned symmetrically around center
        half_spacing = width * L["qr_spacing"] / 2
        wifi_qr_x = width / 2 - half_spacing - (app_qr_size * qr_scale) / 2
        app_qr_x = width / 2 + half_spacing - (app_qr_size * qr_scale) / 2
    else:
        # QR codes with explicit gap between them
        gap = width * L["qr_spacing"]
        total_qr_width = 2 * app_qr_size * qr_scale + gap
        wifi_qr_x = (width - total_qr_width) / 2
        app_qr_x = wifi_qr_x + app_qr_size * qr_scale + gap

    label_y = qr_y + app_qr_size * qr_scale + height * (0.06 if is_landscape else 0.03)
    label_size = max(12, height * (0.030 if is_landscape else 0.018))
    title_y = icon_y + icon_scale * 0.55
    title_size = max(18, height * L["title_size"])
    subtitle_size = max(10, height * L["subtitle_size"])

    # WiFi QR placeholder dimensions
    wifi_placeholder_size = int(app_qr_size * qr_scale)
    wifi_placeholder_x = int(wifi_qr_x)
    wifi_placeholder_y = int(qr_y)

    # Build SVG
    svg = f"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">
  <defs>
    <linearGradient id="bg">
      <stop offset="0%" stop-color="#5a3a20"/>
      <stop offset="100%" stop-color="#5a3a20"/>
    </linearGradient>
    <linearGradient id="pic-bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#f4c896"/>
      <stop offset="100%" stop-color="#e9b584"/>
    </linearGradient>
    <linearGradient id="frame-body" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#faf5eb"/>
      <stop offset="100%" stop-color="#f0e4d0"/>
    </linearGradient>
  </defs>

  <!-- Gradient background -->
  <rect x="0" y="0" width="{width}" height="{height}" fill="url(#bg)"/>

  <!-- PhotoFrame icon -->
  <g transform="translate({icon_x - icon_scale * 0.5}, {icon_y - icon_scale * 0.5}) scale({icon_scale / 512})">
    <rect x="96" y="112" width="320" height="288" rx="20" fill="url(#frame-body)"/>
    <rect x="120" y="136" width="272" height="240" rx="8" fill="url(#pic-bg)"/>
    <circle cx="328" cy="200" r="28" fill="#fef3c7" fill-opacity="0.95"/>
    <path d="M 120 320 Q 200 260 280 300 Q 340 320 392 306 L 392 376 L 120 376 Z"
          fill="#8b5a3c" fill-opacity="0.7"/>
    <path d="M 120 350 Q 180 320 240 336 Q 320 360 392 344 L 392 376 L 120 376 Z"
          fill="#4a3423" fill-opacity="0.85"/>
    <rect x="236" y="400" width="40" height="12" rx="4" fill="#faf5eb" fill-opacity="0.9"/>
  </g>

  <!-- Title -->
  <text x="{width / 2}" y="{title_y + title_size * 1.2}" text-anchor="middle"
        font-family="sans-serif" font-size="{title_size}" font-weight="600"
        fill="#f0e4d0">ESP Frame</text>
  <text x="{width / 2}" y="{title_y + title_size * 1.5 + subtitle_size * 1.8}" text-anchor="middle"
        font-family="sans-serif" font-size="{subtitle_size}"
        fill="#f0e4d0" fill-opacity="0.6">Free and open source ecosystem for ESP32-based e-paper photo frames</text>

  <!-- WiFi QR code placeholder (white background) -->
  <rect x="{wifi_placeholder_x - 4}" y="{wifi_placeholder_y - 4}"
        width="{wifi_placeholder_size + 8}" height="{wifi_placeholder_size + 8}"
        rx="4" fill="white"/>
  <text x="{wifi_placeholder_x + wifi_placeholder_size / 2}" y="{label_y}"
        text-anchor="middle" font-family="sans-serif" font-size="{label_size}"
        fill="#f0e4d0" fill-opacity="0.8">Scan to connect WiFi</text>

  <!-- App download QR code -->
  <rect x="{int(app_qr_x) - 4}" y="{int(qr_y) - 4}"
        width="{wifi_placeholder_size + 8}" height="{wifi_placeholder_size + 8}"
        rx="4" fill="white"/>
  <g transform="translate({int(app_qr_x)}, {int(qr_y)}) scale({qr_scale})">
    <path d="{app_qr_path}" fill="black"/>
  </g>
  <text x="{int(app_qr_x) + wifi_placeholder_size / 2}" y="{label_y}"
        text-anchor="middle" font-family="sans-serif" font-size="{label_size}"
        fill="#f0e4d0" fill-opacity="0.8">Scan to download app</text>

</svg>"""
    return svg, wifi_placeholder_x, wifi_placeholder_y, wifi_placeholder_size


def svg_to_png(svg_path: str, png_path: str, width: int, height: int) -> bool:
    """Convert SVG to PNG using rsvg-convert."""
    rsvg = shutil.which("rsvg-convert")
    if not rsvg:
        print("ERROR: rsvg-convert not found.")
        print("  Ubuntu: sudo apt-get install librsvg2-bin")
        print("  macOS:  brew install librsvg")
        return False

    try:
        subprocess.run(
            [rsvg, "-w", str(width), "-h", str(height), svg_path, "-o", png_path],
            check=True,
            capture_output=True,
        )
        return True
    except subprocess.CalledProcessError as e:
        print(f"ERROR: rsvg-convert failed: {e.stderr.decode()}")
        return False


def _get_process_cli_base(width: int, height: int) -> list:
    """Return base process-cli command args with orientation detection."""
    node = shutil.which("node")
    if not node:
        raise RuntimeError("node not found")

    if not os.path.exists(PROCESS_CLI):
        raise RuntimeError(
            f"process-cli not found at {PROCESS_CLI}\n  Run: cd process-cli && npm ci"
        )

    orientation = "portrait" if height > width else "landscape"
    return [
        node,
        PROCESS_CLI,
        "--placeholder--",  # input path placeholder
        "-d",
        f"{width}x{height}",
        "--orientation",
        orientation,
        "--scale-mode",
        "cover",
    ]


def png_to_epdgz(png_path: str, output_dir: str, width: int, height: int) -> bool:
    """Convert PNG to EPDGZ using process-cli."""
    try:
        cmd = _get_process_cli_base(width, height)
        cmd[2] = png_path  # replace placeholder
        cmd.extend(["-o", output_dir, "--format", "epdgz"])
        subprocess.run(cmd, check=True)
        return True
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return False
    except subprocess.CalledProcessError as e:
        print(f"ERROR: process-cli failed: {e}")
        return False


def generate_setup_complete_svg(width: int, height: int) -> tuple:
    """Generate setup-complete screen SVG with a single centered QR placeholder.
    Returns (svg_content, qr_x, qr_y, qr_size)."""
    is_landscape = width > height
    L = LAYOUT["landscape" if is_landscape else "portrait"]

    icon_scale = min(width, height) * L["icon_size"]
    icon_x = width / 2
    icon_y = height * L["icon_y"]

    title_y = icon_y + icon_scale * 0.55
    title_size = max(18, height * L["title_size"])
    subtitle_size = max(10, height * L["subtitle_size"])

    # Single centered QR code, slightly larger than the OOBE ones
    qr_target = min(width, height) * L["qr_size"] * 1.3
    qr_x = int((width - qr_target) / 2)
    qr_y_pos = int(height * L["qr_y"])
    qr_size = int(qr_target)

    label_y = qr_y_pos + qr_size + height * (0.06 if is_landscape else 0.03)
    label_size = max(12, height * (0.030 if is_landscape else 0.018))

    svg = f"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">
  <defs>
    <linearGradient id="bg">
      <stop offset="0%" stop-color="#5a3a20"/>
      <stop offset="100%" stop-color="#5a3a20"/>
    </linearGradient>
    <linearGradient id="pic-bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#f4c896"/>
      <stop offset="100%" stop-color="#e9b584"/>
    </linearGradient>
    <linearGradient id="frame-body" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#faf5eb"/>
      <stop offset="100%" stop-color="#f0e4d0"/>
    </linearGradient>
  </defs>

  <!-- Background -->
  <rect x="0" y="0" width="{width}" height="{height}" fill="url(#bg)"/>

  <!-- PhotoFrame icon -->
  <g transform="translate({icon_x - icon_scale * 0.5}, {icon_y - icon_scale * 0.5}) scale({icon_scale / 512})">
    <rect x="96" y="112" width="320" height="288" rx="20" fill="url(#frame-body)"/>
    <rect x="120" y="136" width="272" height="240" rx="8" fill="url(#pic-bg)"/>
    <circle cx="328" cy="200" r="28" fill="#fef3c7" fill-opacity="0.95"/>
    <path d="M 120 320 Q 200 260 280 300 Q 340 320 392 306 L 392 376 L 120 376 Z"
          fill="#8b5a3c" fill-opacity="0.7"/>
    <path d="M 120 350 Q 180 320 240 336 Q 320 360 392 344 L 392 376 L 120 376 Z"
          fill="#4a3423" fill-opacity="0.85"/>
    <rect x="236" y="400" width="40" height="12" rx="4" fill="#faf5eb" fill-opacity="0.9"/>
  </g>

  <!-- Title -->
  <text x="{width / 2}" y="{title_y + title_size * 1.2}" text-anchor="middle"
        font-family="sans-serif" font-size="{title_size}" font-weight="600"
        fill="#f0e4d0">Setup Complete!</text>
  <text x="{width / 2}" y="{title_y + title_size * 1.5 + subtitle_size * 1.8}" text-anchor="middle"
        font-family="sans-serif" font-size="{subtitle_size}"
        fill="#f0e4d0" fill-opacity="0.6">Scan the QR code or use the app to get started</text>

  <!-- QR code placeholder (white background, firmware fills at runtime) -->
  <rect x="{qr_x - 4}" y="{qr_y_pos - 4}"
        width="{qr_size + 8}" height="{qr_size + 8}"
        rx="4" fill="white"/>
  <text x="{width / 2}" y="{label_y}"
        text-anchor="middle" font-family="sans-serif" font-size="{label_size}"
        fill="#f0e4d0" fill-opacity="0.8">Scan to open web interface</text>

</svg>"""
    return svg, qr_x, qr_y_pos, qr_size


def generate_meta_header(
    qr_x: int,
    qr_y: int,
    qr_size: int,
    setup_qr_x: int,
    setup_qr_y: int,
    setup_qr_size: int,
    output_path: str,
):
    """Generate C header with QR position metadata for both screens."""
    content = (
        "// Auto-generated splash screen metadata\n"
        "// Do not edit manually\n"
        "\n"
        "// OOBE splash screen - WiFi QR code position\n"
        f"#define SPLASH_WIFI_QR_X {qr_x}\n"
        f"#define SPLASH_WIFI_QR_Y {qr_y}\n"
        f"#define SPLASH_WIFI_QR_SIZE {qr_size}\n"
        "\n"
        "// Setup complete screen - web UI QR code position\n"
        f"#define SETUP_COMPLETE_QR_X {setup_qr_x}\n"
        f"#define SETUP_COMPLETE_QR_Y {setup_qr_y}\n"
        f"#define SETUP_COMPLETE_QR_SIZE {setup_qr_size}\n"
    )
    with open(output_path, "w") as f:
        f.write(content)


def main():
    # Import board dimensions from boards.py
    sys.path.insert(0, os.path.dirname(__file__))
    from boards import BOARD_DIMENSIONS

    parser = argparse.ArgumentParser(description="Generate OOBE splash screen EPDGZ")
    size_group = parser.add_mutually_exclusive_group(required=True)
    size_group.add_argument(
        "--board",
        choices=BOARD_DIMENSIONS.keys(),
        help="Board name (looks up display resolution from boards.py)",
    )
    size_group.add_argument(
        "--size",
        choices=SCREEN_SIZES.keys(),
        help="Display resolution directly (e.g. 800x480)",
    )
    parser.add_argument(
        "--output-dir",
        default=os.path.join(os.path.dirname(__file__), "..", "main", "splash_data"),
        help="Output directory for EPDGZ, PNG preview, and metadata",
    )
    parser.add_argument(
        "--svg-only",
        action="store_true",
        help="Only generate SVG (skip EPDGZ conversion)",
    )
    args = parser.parse_args()

    if args.board:
        w, h = BOARD_DIMENSIONS[args.board]
    else:
        w, h = SCREEN_SIZES[args.size]

    os.makedirs(args.output_dir, exist_ok=True)

    # Generate both splash screens
    screens = [
        ("splash", generate_splash_svg),
        ("setup_complete", generate_setup_complete_svg),
    ]

    all_meta = {}

    for name, gen_func in screens:
        print(f"\n=== {name} ({w}x{h}) ===")

        svg_content, qr_x, qr_y, qr_size = gen_func(w, h)
        all_meta[name] = (qr_x, qr_y, qr_size)
        print(f"  QR placeholder: x={qr_x} y={qr_y} size={qr_size}")

        if args.svg_only:
            svg_path = os.path.join(args.output_dir, f"{name}.svg")
            with open(svg_path, "w") as f:
                f.write(svg_content)
            print(f"  SVG: {svg_path}")
            continue

        with tempfile.NamedTemporaryFile(suffix=".svg", delete=False) as tmp_svg:
            tmp_svg.write(svg_content.encode())
            tmp_svg_path = tmp_svg.name

        png_path = os.path.join(args.output_dir, f"{name}.png")

        try:
            if not svg_to_png(tmp_svg_path, png_path, w, h):
                sys.exit(1)
            print(f"  PNG: {png_path}")

            print(f"  Converting to EPDGZ via process-cli...")
            if not png_to_epdgz(png_path, args.output_dir, w, h):
                print(f"  ERROR: Failed to generate EPDGZ")
                sys.exit(1)

            epdgz_path = os.path.join(args.output_dir, f"{name}.epdgz")
            print(f"  EPDGZ: {epdgz_path}")

            # Clean up thumbnail generated by process-cli
            thumb_path = os.path.join(args.output_dir, f"{name}.jpg")
            if os.path.exists(thumb_path):
                os.unlink(thumb_path)

        finally:
            os.unlink(tmp_svg_path)

    # Write metadata header
    splash_meta = all_meta["splash"]
    setup_meta = all_meta["setup_complete"]
    meta_path = os.path.join(args.output_dir, "splash_meta.h")
    generate_meta_header(
        splash_meta[0],
        splash_meta[1],
        splash_meta[2],
        setup_meta[0],
        setup_meta[1],
        setup_meta[2],
        meta_path,
    )
    print(f"\n  Metadata: {meta_path}")


if __name__ == "__main__":
    main()
