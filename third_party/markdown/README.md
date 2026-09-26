# Browser Markdown adapter

`convert.js` runs in Qt WebEngine against a clone of the rendered DOM. There is no
Node.js service, network dependency, or package installation at runtime.

## Vendored dependencies

Upstream distributions are checked in **unchanged**, with their MIT licenses.
Versions are pinned; upgrades require reviewing upstream changes and rerunning
the browser and service regressions. The adapter/tests use the project's
Apache-2.0 license.

| File | Version | Upstream | Download |
| --- | --- | --- | --- |
| `turndown.js` | 7.2.4 | [mixmark-io/turndown](https://github.com/mixmark-io/turndown/tree/v7.2.4) | [browser distribution](https://unpkg.com/turndown@7.2.4/dist/turndown.js) |
| `turndown-plugin-gfm.js` | 1.0.2 | [mixmark-io/turndown-plugin-gfm](https://github.com/mixmark-io/turndown-plugin-gfm) | [browser distribution](https://unpkg.com/turndown-plugin-gfm@1.0.2/dist/turndown-plugin-gfm.js) |
| `LICENSE.turndown` | 7.2.4 | Turndown MIT license | [license](https://unpkg.com/turndown@7.2.4/LICENSE) |
| `LICENSE.turndown-plugin-gfm` | 1.0.2 | GFM plugin MIT license | [license](https://unpkg.com/turndown-plugin-gfm@1.0.2/LICENSE) |

SHA-256 (downloaded bytes):

```text
c97187f436d41638bf7acf346a39d9d42f2f2c02af18245a297c09e796f8e46f  turndown.js
cf744cc1b7580f06d64ce236a4ff2630a53d389eccf2133a09d71ca443511912  turndown-plugin-gfm.js
a055e3a15db4b4cbb5af381230eba085603126d7d5d8c4dae45ee85b877a9729  LICENSE.turndown
a055e3a15db4b4cbb5af381230eba085603126d7d5d8c4dae45ee85b877a9729  LICENSE.turndown-plugin-gfm
```

## Selection and conversion

1. Clone the live document; capture computed visibility, image `currentSrc`,
   element `baseURI`, checkbox state, code languages, and table alignment.
2. Remove non-content nodes and comments on the clone. Preserve visible
   `aria-hidden` content and descendants that restore `visibility:visible`.
   SVG/canvas output retains available labels/fallback text, with a warning.
   Do not remove code-looking prose through regular-expression cleanup.
3. `conservative` retains the normalized body. `readability` uses the existing
   vendored Readability constructor, a DOM serializer, `keepClasses:true`, and
   `charThreshold:200`. Missing/failed extraction falls back to conservative.
   When structured content dominates a page, losing its code/list/table
   structure also triggers an explicit conservative fallback.
4. `turndown` converts the selected DOM. `legacy_cpp` returns that same selected
   HTML for the existing C++ converter, so selection and conversion remain
   independent choices.

The adapter uses the GFM task/strikethrough extensions and local rules for
fenced code and tables. Code fences exceed every backtick run in the code;
whitespace, blank lines, `<br>` line breaks, and language metadata are retained.
Turndown handles nested lists and inline code.

Simple rectangular tables with a single all-`th` header row become GFM tables.
Spans, captions, nested/block content, missing headers, and other structures
retain sanitized HTML with `table_preserved_as_html`. This avoids inventing
headers or flattening data. Raw table newlines use character references so
blank lines inside `<pre>` cannot prematurely end the CommonMark HTML block.

URLs are resolved before extraction using the live element's base URI.
Links allow HTTP(S), mailto, tel and FTP; images allow HTTP(S) and base64 raster
data URLs (PNG/JPEG/GIF/WebP/AVIF). Active/invalid schemes are removed with
warnings. Table HTML uses a tag/attribute allowlist and URL revalidation;
event handlers, style, script, SVG data and active embedded elements do not
pass through. Literal HTML-like prose is escaped as text; code remains literal.

Lazy image attributes (`data-src`, `data-original`, `data-original-src`,
`data-lazy-src`, `lazy-src`) supply the URL when the selected image is missing,
an inline preview, a tiny image, or a named placeholder. A loaded full-size
`currentSrc` takes precedence.

## Injection protocol

Evaluate both browser libraries, the Readability constructor extracted between
the existing `READABILITY_BEGIN` / `READABILITY_END` markers, then `convert.js`
inside the same function in `QWebEngineScript::ApplicationWorld`.
The entry file is an IIFE expression and starts with `(` so it can be prefixed
with `return ` when wrapped with the libraries.

Input:

```js
window.__seimiMarkdownOptions = {
  algorithm: 'conservative', // or readability
  converter: 'turndown'      // or legacy_cpp
};
```

The return value is JSON with `ok`, `totalChunks`, `totalChars`, `title`, actual
`extractor` and `converter`, `warnings`, `fallback_reason`, `conversion_ms`, and
`error` on failure. Read `window.__seimiMarkdown.chunks` sequentially and verify
the advertised character count. Empty output has zero chunks and zero length.
Chunks contain at most 30,000 UTF-16 units, never split a surrogate pair, and
the entire payload is capped at 32 Mi UTF-16 units. Failure clears stale chunks.
The worker owns automatic converter fallback and combines its diagnostics with
these extraction diagnostics.

## Regression tests

```sh
python third_party/markdown/test_markdown.py
python third_party/markdown/test_markdown.py --browser /path/to/chromium
```

The harness launches a disposable headless Chrome/Chromium profile and needs
only Python's standard library plus a local browser. It does not touch a user's
browser profile. `tests.js` exercises actual browser DOM/CSS behavior and the
same injected sources: DOM immutability, visibility, dynamic images/checkboxes,
relative URLs, entity/Unicode fidelity, code fences and blank lines, nested
lists, GFM/complex tables, sanitization, Readability success/fallback, a >64 KiB
document, surrogate boundaries, explicit failures and the payload size cap.
It also executes the retained `readability/simplify.js` directly, verifying
that legitimate code and visible `aria-hidden` content survive cleanup.
Use `--legacy-simplify /path/to/old/simplify.js` to compare a previous version;
the unfixed version must fail those regressions.

After building seimi-render, run the HTTP/WebSocket/MCP integration suite from
the repository root (requires Python `requests` and `websocket-client`):

```sh
python scripts/test_markdown_api.py --binary build/seimi-render
# Windows: --binary build/seimi-render.exe --qt-bin C:/Qt/6.7.2/msvc2019_64/bin
```

It uses local fixtures, temporary ports/data, and checks converter selection,
Unicode chunk transfer, isolated script globals, combined HTML/PDF/screenshot
output, protocol diagnostics, and worker reuse after a preparation timeout.
The standalone legacy C++ regressions are in `scripts/test_html2md.cpp`.
