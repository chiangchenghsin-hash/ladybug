#!/usr/bin/env python3
import argparse
import ssl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse


class RangeFileHandler(BaseHTTPRequestHandler):
    root = Path(".").resolve()
    strip_prefix = ""

    def translate_path(self):
        path = unquote(urlparse(self.path).path).lstrip("/")
        if self.strip_prefix and path == self.strip_prefix:
            path = ""
        elif self.strip_prefix and path.startswith(self.strip_prefix + "/"):
            path = path[len(self.strip_prefix) + 1:]
        return (self.root / path).resolve()

    def send_file(self, head_only=False):
        file_path = self.translate_path()
        try:
            file_path.relative_to(self.root)
        except ValueError:
            self.send_error(403)
            return
        if not file_path.is_file():
            self.send_error(404)
            return

        size = file_path.stat().st_size
        start = 0
        end = size - 1
        status = 200
        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            status = 206
            range_spec = range_header[len("bytes="):].split(",", 1)[0]
            first, _, last = range_spec.partition("-")
            if first:
                start = int(first)
                if last:
                    end = int(last)
            elif last:
                suffix_length = int(last)
                start = max(size - suffix_length, 0)
                end = size - 1
            end = min(end, size - 1)
            if start > end or start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return

        length = end - start + 1 if size else 0
        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head_only:
            return
        with file_path.open("rb") as file:
            file.seek(start)
            remaining = length
            while remaining:
                chunk = file.read(min(1024 * 64, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def do_GET(self):
        self.send_file(False)

    def do_HEAD(self):
        self.send_file(True)

    def log_message(self, fmt, *args):
        if self.server.quiet:
            return
        super().log_message(fmt, *args)


class RangeFileServer(ThreadingHTTPServer):
    quiet = False


def main():
    parser = argparse.ArgumentParser(
        description="Serve local files with HEAD and byte-range GET support.")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--root", default=".")
    parser.add_argument(
        "--strip-prefix",
        default="",
        help="Drop this first URL path component before mapping to --root. "
        "Useful for path-style S3 bucket names.",
    )
    parser.add_argument("--cert", help="TLS certificate PEM for HTTPS.")
    parser.add_argument("--key", help="TLS private key PEM for HTTPS.")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    RangeFileHandler.root = Path(args.root).resolve()
    RangeFileHandler.strip_prefix = args.strip_prefix.strip("/")
    server = RangeFileServer((args.bind, args.port), RangeFileHandler)
    server.quiet = args.quiet
    if args.cert or args.key:
        if not args.cert or not args.key:
            parser.error("--cert and --key must be provided together")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    scheme = "https" if args.cert else "http"
    print(f"Serving {RangeFileHandler.root} at {scheme}://{args.bind}:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
