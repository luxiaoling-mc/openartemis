#!/usr/bin/env python3
"""openartemis WASM 本地预览服务器.

用法:
    python web/serve.py [DIR] [PORT]        # 默认 . 8080

打开:
    http://localhost:8080/index.html?pfs=root.pfs

说明:
  * 本项目是单线程 wasm(platform_wasm.cpp 把 OA_EMOTE_THREADS 钉成 1),
    不需要 SharedArrayBuffer,因此不强制 COOP/COEP;这里仍然带上这两个头,
    万一以后开了 -pthread 也能直接跑。
  * .wasm 必须返回 application/wasm,否则浏览器会拒绝流式编译(index.js 会
    退回 ArrayBuffer 实例化,慢但仍能跑)。
  * 存档在 /save(IDBFS -> IndexedDB),跟服务器目录无关;换浏览器配置会清空。
"""
import functools
import http.server
import os
import socketserver
import sys

MIME = {
    ".wasm": "application/wasm",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".html": "text/html; charset=utf-8",
    ".data": "application/octet-stream",
    ".pfs": "application/octet-stream",
    ".xp3": "application/octet-stream",
    ".json": "application/json",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".ogg": "audio/ogg",
    ".mp4": "video/mp4",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    def guess_type(self, path):
        ext = os.path.splitext(path)[1].lower()
        return MIME.get(ext) or super().guess_type(path)

    def end_headers(self):
        # 为将来的 pthread 构建预留(单线程构建无害)
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("[serve] " + (fmt % args) + "\n")


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    handler = functools.partial(Handler, directory=directory)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", port), handler) as httpd:
        print(f"[serve] {os.path.abspath(directory)} -> http://localhost:{port}/index.html")
        print(f"[serve] 例: http://localhost:{port}/index.html?pfs=root.pfs")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[serve] bye")


if __name__ == "__main__":
    main()
