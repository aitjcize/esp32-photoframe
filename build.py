#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys

# Add scripts to sys.path to import boards
sys.path.append(os.path.join(os.path.dirname(__file__), "scripts"))
from boards import BOARD_TARGET, SUPPORTED_BOARDS

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
        print(
            "  ✗ 'npm' not found. Please ensure Node.js is installed and in your PATH."
        )
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


def idf_py_command():
    """Return the argv prefix that runs idf.py.

    On Windows, CreateProcess cannot execute the idf.py script directly
    (WinError 193). Prefer the idf.py.exe wrapper that the Windows installers
    put on PATH; otherwise run $IDF_PATH/tools/idf.py through the ESP-IDF
    Python interpreter (IDF_PYTHON_ENV_PATH is set by the activation script).
    """
    if os.name != "nt":
        return ["idf.py"]
    found = shutil.which("idf.py")
    if found and found.lower().endswith(".exe"):
        return [found]
    idf_path = os.environ.get("IDF_PATH")
    script = os.path.join(idf_path, "tools", "idf.py") if idf_path else found
    if not script or not os.path.isfile(script):
        return ["idf.py"]  # let subprocess raise FileNotFoundError
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    python = os.path.join(env_path, "Scripts", "python.exe") if env_path else ""
    if not os.path.isfile(python):
        python = sys.executable
    return [python, script]


def build_firmware(board, extra_args, debug=False):
    """Build firmware with idf.py."""
    print(f"\n=== Building firmware for {board}{' [debug]' if debug else ''} ===")
    sdkconfig_defaults = f"sdkconfig.defaults;boards/sdkconfig.defaults.{board}"
    if debug:
        # Debug-only overlay: core-dump-to-flash capture (+ the coredump partition
        # from generate_partitions.py). Changes the partition table — never used
        # for release or demo builds.
        sdkconfig_defaults += ";sdkconfig.defaults.debug"

    # Boards differ in target chip (the M5Paper is a plain ESP32, everything
    # else is an ESP32-S3). Pass it explicitly: idf.py resolves the target
    # before it merges the defaults files, so CONFIG_IDF_TARGET in the board's
    # sdkconfig.defaults alone is not enough on a clean tree.
    target = BOARD_TARGET[board]

    idf_base = idf_py_command() + [
        f"-DIDF_TARGET={target}",
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
        print(
            "Error: 'idf.py' not found. Please ensure ESP-IDF is correctly installed and activated."
        )
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
        "--debug",
        action="store_true",
        help="Debug build: enable core-dump-to-flash capture. Changes the "
        "partition table (adds a coredump partition) — do not ship to users.",
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
        build_firmware(args.board, extra_args, debug=args.debug)


if __name__ == "__main__":
    main()
