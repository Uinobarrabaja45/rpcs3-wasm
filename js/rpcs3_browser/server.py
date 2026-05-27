#!/usr/bin/env python3
import http.server
import socketserver
import sys

class CORSRequestHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # COOP/COEP are required for SharedArrayBuffer, which the WASM build
        # needs for PPU/SPU JIT worker threads.
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        super().end_headers()

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    with socketserver.TCPServer(("", port), CORSRequestHandler) as httpd:
        print(f"Serving HTTP on :: port {port} (http://[::]:{port}/)")
        print("COOP/COEP headers enabled for SharedArrayBuffer")
        httpd.serve_forever()
