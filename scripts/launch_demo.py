#!/usr/bin/env python3
"""
Launch the ESP32 PhotoFrame demo page locally.

This script:
1. Builds firmware with idf.py build (optional)
2. Downloads latest stable release from GitHub
3. Builds the Vue demo webapp (npm run build:demo)
4. Copies sample.jpg to demo/
5. Generates manifests for both stable and dev versions
6. Starts a local web server

Usage:
    python launch_demo.py              # Build, download, and serve
    python launch_demo.py --port 8080  # Custom port
    python launch_demo.py --skip-build # Skip building firmware
    python launch_demo.py --dev        # Use Vite dev server instead of static
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import urllib.request
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path


class CORSRequestHandler(SimpleHTTPRequestHandler):
    """HTTP request handler with CORS headers."""

    def end_headers(self):
        # Add CORS headers
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()


def find_available_port(start_port=8000, max_attempts=10):
    """Find an available port starting from start_port."""
    for port in range(start_port, start_port + max_attempts):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("localhost", port))
                return port
            except socket.error:
                continue
    return None


def build_firmware(project_root, board="waveshare_photopainter_73"):
    """Build firmware using idf.py build."""

    print(f"\nBuilding firmware for {board}...")
    print("  This may take a few minutes...")

    try:
        result = subprocess.run(
            [
                "idf.py",
                "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults." + board,
                "build",
            ],
            cwd=project_root,
            capture_output=True,
            text=True,
            check=True,
        )

        print(f"  ✓ {board} firmware built successfully")
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Error building firmware: {e}")
        if e.stderr:
            # Print last few lines of error
            lines = e.stderr.strip().split("\n")
            for line in lines[-10:]:
                print(f"    {line}")
        return False


def download_stable_firmware(demo_dir, project_root):
    """Download latest stable release firmware from GitHub for all boards."""

    print("\nDownloading stable release firmware...")

    BOARDS = ["waveshare_photopainter_73", "seeedstudio_xiao_ee02"]
    success_count = 0

    try:
        # Get latest tag
        result = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0"],
            cwd=project_root,
            capture_output=True,
            text=True,
            check=False,
        )

        if result.returncode != 0:
            print("  ⚠ Warning: No git tags found, skipping stable firmware download")
            return False

        latest_tag = result.stdout.strip()
        print(f"  Latest release: {latest_tag}")

        # Get repository info (always aitjcize/esp32-photoframe)
        repo_path = "aitjcize/esp32-photoframe"

        for board in BOARDS:
            board_dir = demo_dir / board
            board_dir.mkdir(parents=True, exist_ok=True)

            download_url = f"https://github.com/{repo_path}/releases/download/{latest_tag}/photoframe-firmware-{board}-merged.bin"
            output_file = board_dir / f"photoframe-firmware-{board}-merged.bin"

            print(f"  Downloading {board} from: {download_url}")

            try:
                urllib.request.urlretrieve(download_url, output_file)
                print(f"  ✓ Downloaded stable firmware for {board} ({latest_tag})")
                success_count += 1
            except Exception as e:
                # Fallback for waveshare backward compatibility (before board names were in filename)
                if board == "waveshare_photopainter_73":
                    try:
                        print(
                            f"  ⚠ Warning: Could not download board-specific firmware, trying legacy filename..."
                        )
                        legacy_url = f"https://github.com/{repo_path}/releases/download/{latest_tag}/photoframe-firmware-merged.bin"
                        urllib.request.urlretrieve(legacy_url, output_file)
                        print(
                            f"  ✓ Downloaded legacy stable firmware for {board} ({latest_tag})"
                        )
                        success_count += 1
                        continue
                    except Exception as e2:
                        pass

                print(
                    f"  ⚠ Warning: Could not download stable firmware for {board}: {e}"
                )

        return success_count == len(BOARDS)

    except Exception as e:
        print(f"  ⚠ Warning: Error downloading stable firmware: {e}")
        return False


def build_demo_webapp(project_root):
    """Build the Vue demo webapp."""

    print("\nBuilding demo webapp...")

    webapp_dir = project_root / "webapp"

    if not webapp_dir.exists():
        print(f"  ✗ Error: {webapp_dir} not found")
        return False

    try:
        # Install dependencies
        print("  Installing dependencies...")
        subprocess.run(
            ["npm", "i"],
            cwd=webapp_dir,
            capture_output=True,
            text=True,
            check=True,
        )

        # Build demo
        print("  Building demo...")
        subprocess.run(
            ["npm", "run", "build:demo"],
            cwd=webapp_dir,
            capture_output=True,
            text=True,
            check=True,
        )

        print("  ✓ Demo webapp built successfully")
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Error building demo webapp: {e}")
        if e.stderr:
            for line in e.stderr.strip().split("\n")[-10:]:
                print(f"    {line}")
        return False


def copy_required_files(demo_dir, project_root):
    """Copy required files for the demo page."""

    print("\nCopying required files...")

    # Copy sample image
    src_img = project_root / ".img" / "sample.jpg"
    dst_img = demo_dir / "sample.jpg"

    if src_img.exists():
        shutil.copy2(src_img, dst_img)
        print(f"  ✓ Copied {src_img.name}")
    else:
        print(f"  ⚠ Warning: {src_img} not found")


def generate_manifests(project_root, boards=None):
    """Run the manifest generation script for all boards."""

    if boards is None:
        boards = ["waveshare_photopainter_73", "seeedstudio_xiao_ee02"]

    print("\nGenerating manifests...")
    demo_dir = project_root / "demo"
    manifest_script = project_root / "scripts" / "generate_manifests.py"

    if not manifest_script.exists():
        print(f"  ✗ Error: {manifest_script} not found")
        return False

    success = True
    for board in boards:
        board_dir = demo_dir / board
        board_dir.mkdir(parents=True, exist_ok=True)

        try:
            # Run manifest generation with --dev and --no-copy flags
            result = subprocess.run(
                [
                    "python3",
                    str(manifest_script),
                    "--dev",
                    "--no-copy",
                    "--demo-dir",
                    f"demo/{board}",
                    "--board",
                    board,
                ],
                cwd=project_root,
                capture_output=True,
                text=True,
                check=True,
            )

            # Print output
            if result.stdout:
                for line in result.stdout.strip().split("\n"):
                    print(f"  [{board}] {line}")

        except subprocess.CalledProcessError as e:
            print(f"  ✗ Error generating manifests for {board}: {e}")
            if e.stderr:
                print(e.stderr)
            success = False

    # Link first requested board as default at root
    default_board = boards[0]
    if (demo_dir / default_board / "manifest.json").exists():
        shutil.copy2(
            demo_dir / default_board / "manifest.json", demo_dir / "manifest.json"
        )
        shutil.copy2(
            demo_dir / default_board / "manifest-dev.json",
            demo_dir / "manifest-dev.json",
        )

    return success


def serve_demo(demo_dir, port=8000):
    """Start local web server to serve the demo page.

    The demo is built with base="/esp32-photoframe/" for GitHub Pages.
    To serve locally, we serve from the parent directory and create
    a symlink so /esp32-photoframe/ maps to the demo folder.
    """

    project_root = demo_dir.parent
    symlink_path = project_root / "esp32-photoframe"

    # Create symlink: project_root/esp32-photoframe -> demo/
    # This allows /esp32-photoframe/assets/* to resolve correctly
    if not symlink_path.exists():
        symlink_path.symlink_to(demo_dir.name)
        print(f"  Created symlink: esp32-photoframe -> {demo_dir.name}")
    elif symlink_path.is_symlink():
        # Symlink exists, verify it points to demo
        pass
    else:
        print(f"  ⚠ Warning: {symlink_path} exists but is not a symlink")

    os.chdir(project_root)

    actual_port = find_available_port(port)
    if not actual_port:
        print(f"  ✗ Error: Could not find an available port starting from {port}")
        return

    server = HTTPServer(("localhost", actual_port), CORSRequestHandler)

    print("\n" + "=" * 60)
    print("ESP32 PhotoFrame Demo Page")
    print("=" * 60)
    print(f"\nDemo page available at:")
    print(f"  http://localhost:{actual_port}/esp32-photoframe/#demo")
    print(f"\nWeb flasher available at:")
    print(f"  http://localhost:{actual_port}/esp32-photoframe/#flash")
    print(f"\nPress Ctrl+C to stop the server")
    print("=" * 60 + "\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()


def main():
    parser = argparse.ArgumentParser(
        description="Launch ESP32 PhotoFrame demo page locally"
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8000,
        help="Port for local web server (default: 8000)",
    )
    parser.add_argument(
        "--skip-build", action="store_true", help="Skip building firmware"
    )
    parser.add_argument(
        "--skip-download", action="store_true", help="Skip downloading stable firmware"
    )
    parser.add_argument(
        "--skip-copy", action="store_true", help="Skip copying required files"
    )
    parser.add_argument(
        "--skip-manifests", action="store_true", help="Skip generating manifests"
    )
    parser.add_argument(
        "--skip-webapp", action="store_true", help="Skip building demo webapp"
    )
    parser.add_argument(
        "--dev",
        action="store_true",
        help="Use Vite dev server instead of building and serving static files",
    )
    parser.add_argument(
        "--board",
        default="waveshare_photopainter_73",
        choices=["waveshare_photopainter_73", "seeedstudio_xiao_ee02", "all"],
        help="Board type to build (default: waveshare_photopainter_73)",
    )

    args = parser.parse_args()

    # Get paths
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    demo_dir = project_root / "demo"

    print("=" * 60)
    print("ESP32 PhotoFrame Demo Launcher")
    print("=" * 60)

    # Determine boards to handle
    all_boards = ["waveshare_photopainter_73", "seeedstudio_xiao_ee02"]
    selected_boards = [args.board] if args.board != "all" else all_boards

    # Build firmware
    if not args.skip_build:
        for board in selected_boards:
            if not build_firmware(project_root, board):
                print(f"\n⚠ Warning: Firmware build for {board} failed")
                response = input("Continue anyway? (y/n): ")
                if response.lower() != "y":
                    sys.exit(1)

    # Copy dev firmware from build to demo
    if not args.skip_build:
        print("\nCopying dev firmware...")

        for board in selected_boards:
            board_dir = demo_dir / board
            board_dir.mkdir(parents=True, exist_ok=True)

            # Use generate_manifests.py to copy and merge firmware
            try:
                subprocess.run(
                    [
                        "python3",
                        str(project_root / "scripts" / "generate_manifests.py"),
                        "--board",
                        board,
                        "--demo-dir",
                        f"demo/{board}",
                    ],
                    cwd=project_root,
                    check=True,
                    capture_output=True,
                )

                # Copy merged firmware as dev version
                src_firmware = board_dir / f"photoframe-firmware-{board}-merged.bin"
                dst_firmware = board_dir / f"photoframe-firmware-{board}-dev.bin"
                if src_firmware.exists():
                    shutil.copy2(src_firmware, dst_firmware)
                    print(f"  ✓ Copied dev firmware for {board}")
            except Exception as e:
                print(f"  ⚠ Warning: Could not copy dev firmware for {board}: {e}")

    # Download stable firmware (process ALL boards so demo works)
    if not args.skip_download:
        download_stable_firmware(demo_dir, project_root)

    # Copy required files
    if not args.skip_copy:
        copy_required_files(demo_dir, project_root)

    # Generate manifests (process ALL boards so demo works)
    if not args.skip_manifests:
        if not generate_manifests(project_root, boards=all_boards):
            print("\n⚠ Warning: Manifest generation failed, but continuing...")

    # Handle --dev mode: use Vite dev server
    if args.dev:
        print("\n" + "=" * 60)
        print("Starting Vite dev server...")
        print("=" * 60)
        webapp_dir = project_root / "webapp"
        try:
            subprocess.run(
                ["npm", "run", "dev:demo"],
                cwd=webapp_dir,
                check=True,
            )
        except KeyboardInterrupt:
            print("\nShutting down dev server...")
        return

    # Build demo webapp (for static serving)
    if not args.skip_webapp:
        if not build_demo_webapp(project_root):
            print("\n⚠ Warning: Demo webapp build failed")
            response = input("Continue anyway? (y/n): ")
            if response.lower() != "y":
                sys.exit(1)

    # Serve the demo
    serve_demo(demo_dir, args.port)


if __name__ == "__main__":
    main()
