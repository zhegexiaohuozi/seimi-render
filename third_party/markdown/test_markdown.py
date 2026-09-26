#!/usr/bin/env python3
"""Exercise the injected adapter in a real browser; no npm/runtime dependency.

Usage: python third_party/markdown/test_markdown.py [--browser /path/to/chrome]
"""
import argparse
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser')
    parser.add_argument('--legacy-simplify', type=Path,
                        help='Override the old simplifier source for regression comparison')
    args = parser.parse_args()
    candidates = [args.browser, shutil.which('chromium'), shutil.which('chromium-browser'),
                  shutil.which('google-chrome'),
                  'C:/Program Files/Google/Chrome/Application/chrome.exe',
                  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
                  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']
    browser = next((path for path in candidates if path and Path(path).is_file()), None)
    if not browser:
        parser.error('Chrome/Chromium not found; pass --browser explicitly')
    directory = Path(__file__).resolve().parent
    readability = (directory.parent / 'readability/extract.js').read_text(encoding='utf-8')
    readability = readability.split('// [[[ READABILITY_BEGIN ]]]', 1)[1].split('// [[[ READABILITY_END ]]]', 1)[0]
    sources = {name: (directory / name).read_text(encoding='utf-8') for name in
               ['turndown.js', 'turndown-plugin-gfm.js', 'convert.js']}
    sources['readability'] = readability
    simplify = args.legacy_simplify or directory.parent / 'readability/simplify.js'
    sources['simplify'] = simplify.read_text(encoding='utf-8')
    source_json = json.dumps(sources).replace('</', '<\\/')
    tests = (directory / 'tests.js').read_text(encoding='utf-8').replace('</', '<\\/')
    page = '<!doctype html><meta charset="utf-8"><script>window.testSources=' + source_json + ';' + tests + '</script>'
    with tempfile.TemporaryDirectory(prefix='seimi-markdown-') as temp:
        path = Path(temp) / 'test.html'
        path.write_text(page, encoding='utf-8')
        command = [browser, '--headless', '--disable-gpu', '--disable-extensions',
                   '--no-first-run', '--no-default-browser-check', '--dump-dom',
                   '--user-data-dir=' + str(Path(temp) / 'profile'), path.as_uri()]
        if hasattr(os, 'geteuid') and os.geteuid() == 0:
            command.append('--no-sandbox')
        result = subprocess.run(command, capture_output=True, timeout=60, encoding='utf-8', errors='replace')
        match = re.search(r'<pre id="test-results">(.*?)</pre>', result.stdout, re.S)
        if not match:
            raise SystemExit('Browser did not emit results:\n' + result.stdout[-2000:] + result.stderr[-2000:])
        results = json.loads(html.unescape(match.group(1)))
    for test in results:
        print(('PASS ' if test['ok'] else 'FAIL ') + test['name'] + (': ' + test['error'] if not test['ok'] else ''))
    passed = sum(test['ok'] for test in results)
    print(f'{passed}/{len(results)} browser regression cases passed')
    raise SystemExit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
