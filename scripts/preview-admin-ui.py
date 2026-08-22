#!/usr/bin/env python3
"""admin-ui 本地预览服务器（标准库实现，模拟 seimi-render HttpServer 的静态路由）。

真实服务的路由映射：
  GET /            -> admin-ui/index.html
  GET /ui/<file>   -> admin-ui/<file>   （页面内资源都带 /ui/ 绝对前缀）

直接 `python -m http.server --directory admin-ui` 预览会因 /ui/* 404 丢失全部样式与脚本，
必须用本脚本（或等价映射）预览。

用法：python scripts/preview-admin-ui.py [端口]   # 默认 8799
"""
import http.server
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'admin-ui')
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8799


class AdminUiHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def translate_path(self, path):
        path = path.split('?', 1)[0]
        if path == '/ui':
            path = '/'
        elif path.startswith('/ui/'):
            path = path[3:]  # '/ui/app.css' -> '/app.css'
        return super().translate_path(path)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')  # 开发预览：改完刷新即生效
        super().end_headers()


if __name__ == '__main__':
    with http.server.ThreadingHTTPServer(('127.0.0.1', PORT), AdminUiHandler) as srv:
        print(f'admin-ui preview: http://127.0.0.1:{PORT}/  (root: {ROOT})')
        srv.serve_forever()
