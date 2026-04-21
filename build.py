#!/usr/bin/env python3
import argparse
import subprocess
import sys
import os

# Add scripts to sys.path to import boards
sys.path.append(os.path.join(os.path.dirname(__file__), "scripts"))
from boards import SUPPORTED_BOARDS

BOARDS = list(SUPPORTED_BOARDS.keys())

STEPS = ["webapp", "splash", "firmware"]


def build_webapp():
    """Build the webapp (npm install + npm run build)."""
    print("\n=== Building webapp ===")
    try:
        subprocess.run("npm install", shell=True, check=True, cwd="webapp")
        subprocess.run("npm run build", shell=True, check=True, cwd="webapp")
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Webapp build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("  ✗ 'npm' not found. Please ensure Node.js is installed and in your PATH.")
        sys.exit(1)


def generate_splash(board):
    """Generate splash screen EPDGZ for the target board."""
    print(f"\n=== Generating splash screen for {board} ===", flush=True)
    output_dir = os.path.join(os.path.dirname(__file__), "main", "splash_data")
    script = os.path.join(os.path.dirname(__file__), "scripts", "generate_splash.py")
    process_cli_dir = os.path.join(os.path.dirname(__file__), "process-cli")

    # Ensure process-cli dependencies are installed
    node_modules = os.path.join(process_cli_dir, "node_modules")
    if not os.path.isdir(node_modules):
        print("  Installing process-cli dependencies...")
        try:
            subprocess.run("npm ci", shell=True, check=True, cwd=process_cli_dir)
        except subprocess.CalledProcessError as e:
            print(f"  ✗ npm ci failed in process-cli with exit code {e.returncode}")
            sys.exit(e.returncode)

    try:
        subprocess.run(
            [sys.executable, script, "--board", board, "--output-dir", output_dir],
            check=True,
        )
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Splash generation failed with exit code {e.returncode}")
        sys.exit(e.returncode)


def build_firmware(board, extra_args):
    """Build firmware with idf.py."""
    print(f"\n=== Building firmware for {board} ===")
    sdkconfig_defaults = f"sdkconfig.defaults;boards/sdkconfig.defaults.{board}"

    idf_base = [
        "idf.py",
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
    ]

    cmake_defines = [a for a in extra_args if a.startswith("-D")]
    post_build_args = [a for a in extra_args if not a.startswith("-D")]

    build_cmd = idf_base + cmake_defines + ["build"]
    print(f"Running: {' '.join(build_cmd)}")

    try:
        subprocess.run(build_cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("Error: 'idf.py' not found. Please ensure ESP-IDF is correctly installed and activated.")
        sys.exit(1)

    # Run post-build commands (flash, monitor, etc.)
    if post_build_args:
        post_cmd = idf_base + post_build_args
        print(f"Running: {' '.join(post_cmd)}")
        try:
            subprocess.run(post_cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Post-build command failed with exit code {e.returncode}")
            sys.exit(e.returncode)


def main():
    parser = argparse.ArgumentParser(description="Build firmware for different boards")
    parser.add_argument(
        "--board",
        choices=BOARDS,
        default="waveshare_photopainter_73",
        help="Board type to build",
    )
    parser.add_argument(
        "--fullclean",
        action="store_true",
        help="Remove sdkconfig and run idf.py fullclean before building",
    )
    parser.add_argument(
        "--step",
        choices=STEPS,
        action="append",
        help="Run only specific step(s). Can be specified multiple times. "
             "If omitted, all steps run.",
    )
    # Allow passing extra arguments to idf.py
    args, extra_args = parser.parse_known_args()

    steps = args.step if args.step else STEPS

    if args.fullclean:
        print("Performing full clean...")
        import shutil
        for f in ["sdkconfig", "partitions.csv"]:
            if os.path.exists(f):
                os.remove(f)
                print(f"  ✓ Removed {f}")
        if os.path.isdir("build"):
            shutil.rmtree("build")
            print("  ✓ Removed build/")

    if "webapp" in steps:
        build_webapp()

    if "splash" in steps:
        generate_splash(args.board)

    if "firmware" in steps:
        build_firmware(args.board, extra_args)


if __name__ == "__main__":
    main()
