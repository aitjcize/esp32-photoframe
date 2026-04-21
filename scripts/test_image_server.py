#!/usr/bin/env python3
"""Minimal image server for testing the firmware's ETag/304 logic.

Serves a single image file on every GET, with an ETag derived from the
file's current mtime + size. Re-running with a different file (or touching
the existing one) changes the ETag, so the firmware's next conditional
request will receive 200 + new body instead of 304.

Usage:
    python3 scripts/test_image_server.py path/to/image.jpg
    python3 scripts/test_image_server.py path/to/image.jpg --port 9000
"""

import argparse
import hashlib
import mimetypes
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def compute_etag(path: str) -> str:
    st = os.stat(path)
    # mtime_ns+size is enough; hashing keeps the header compact and opaque.
    raw = f"{st.st_mtime_ns}-{st.st_size}".encode()
    return '"' + hashlib.sha1(raw).hexdigest()[:16] + '"'


def guess_content_type(path: str) -> str:
    mime, _ = mimetypes.guess_type(path)
    return mime or "application/octet-stream"


def make_handler(image_path: str):
    class Handler(BaseHTTPRequestHandler):
        def _serve(self, include_body: bool) -> None:
            try:
                etag = compute_etag(image_path)
            except FileNotFoundError:
                self.send_error(404, "image file missing")
                return

            if_none_match = self.headers.get("If-None-Match", "")
            print(
                f"[{self.log_date_time_string()}] {self.command} {self.path} "
                f"If-None-Match={if_none_match or '(none)'} -> ",
                end="",
            )

            if if_none_match and if_none_match == etag:
                print("304 Not Modified")
                self.send_response(304)
                self.send_header("ETag", etag)
                self.end_headers()
                return

            with open(image_path, "rb") as f:
                body = f.read()

            content_type = guess_content_type(image_path)
            print(f"200 OK ({len(body)} bytes, {content_type}, ETag={etag})")
            self.send_response(200)
            self.send_header("ETag", etag)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if include_body:
                self.wfile.write(body)

        def do_GET(self):
            self._serve(include_body=True)

        def do_HEAD(self):
            self._serve(include_body=False)

        def log_message(self, format, *args):
            # Silence default per-request logging; _serve prints its own line.
            pass

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="Path to the image file to serve")
    parser.add_argument(
        "--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)"
    )
    parser.add_argument("--port", type=int, default=8080, help="Port (default: 8080)")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print(f"error: {args.image} is not a file", file=sys.stderr)
        return 1

    image_path = os.path.abspath(args.image)
    etag = compute_etag(image_path)
    print(f"Serving {image_path}")
    print(f"Initial ETag: {etag}")
    print(f"Listening on http://{args.host}:{args.port}/")
    print("Replace or `touch` the file to invalidate the ETag.")

    server = ThreadingHTTPServer((args.host, args.port), make_handler(image_path))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
