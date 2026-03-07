#!/usr/bin/env python3
import argparse
import subprocess
import sys
import os

# Add scripts to sys.path to import boards
sys.path.append(os.path.join(os.path.dirname(__file__), "scripts"))
from boards import SUPPORTED_BOARDS

BOARDS = list(SUPPORTED_BOARDS.keys())

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
    # Allow passing extra arguments to idf.py
    args, extra_args = parser.parse_known_args()

    board = args.board

    if args.fullclean:
        print("Performing full clean...")
        for f in ["sdkconfig", "partitions.csv"]:
            if os.path.exists(f):
                os.remove(f)
                print(f"  ✓ Removed {f}")

        try:
            subprocess.run(["idf.py", "fullclean"], check=True)
            print("  ✓ idf.py fullclean completed")
        except subprocess.CalledProcessError as e:
            print(f"  ✗ idf.py fullclean failed with exit code {e.returncode}")
            sys.exit(e.returncode)

    sdkconfig_defaults = f"sdkconfig.defaults;sdkconfig.defaults.{board}"

    try:
        subprocess.run("npm run build", shell=True, check=True, cwd="webapp")
    except subprocess.CalledProcessError as e:
        print(f"  ✗ npm run build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("  ✗ 'npm' not found. Please ensure Node.js is installed and in your PATH.")
        sys.exit(1)

    cmd = [
        "idf.py",
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
    ]

    # Add build command if no other command is provided
    if not extra_args:
        cmd.append("build")
    else:
        cmd.extend(extra_args)

    print(f"Building for board: {board}")
    print(f"Running: {' '.join(cmd)}")
    
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("Error: 'idf.py' not found. Please ensure ESP-IDF is correctly installed and activated.")
        print("Hint: Try adding ~/Work/esp/esp-idf/tools to your PATH.")
        sys.exit(1)

if __name__ == "__main__":
    main()
