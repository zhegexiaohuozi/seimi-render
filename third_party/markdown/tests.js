// Loaded only by test_markdown.py in a temporary, real Chromium page.
(function () {
  'use strict';
  var results = [];
  function assert(condition, message) { if (!condition) throw new Error(message); }
  function contains(text, expected) { assert(text.includes(expected), JSON.stringify(expected) + ' absent from ' + text.slice(0, 2000)); }
  function absent(text, unexpected) { assert(!text.includes(unexpected), JSON.stringify(unexpected) + ' present in ' + text.slice(0, 2000)); }
  function page(body, head) {
    document.head.innerHTML = '<title>Fixture title</title><base href="https://example.com/docs/">' + (head || '');
    if (!document.body) document.documentElement.appendChild(document.createElement('body'));
    document.body.innerHTML = body;
  }
  function convert(options, noReadability) {
    window.__seimiMarkdownOptions = options || {};
    var before = document.documentElement.outerHTML;
    var script = testSources['turndown.js'] + '\n' + testSources['turndown-plugin-gfm.js'] + '\n' +
      (noReadability ? '' : testSources.readability) + '\nreturn ' + testSources['convert.js'].replace(/^\/\*[\s\S]*?\*\/\s*/, '');
    var status = JSON.parse(new Function(script)());
    assert(document.documentElement.outerHTML === before, 'live DOM mutated');
    var text = window.__seimiMarkdown.chunks.join('');
    assert(status.totalChars === text.length, 'payload length differs');
    assert(status.totalChunks === window.__seimiMarkdown.chunks.length, 'chunk count differs');
    return { text: text, status: status };
  }
  function test(name, run) {
    try { run(); results.push({name: name, ok: true}); }
    catch (error) { results.push({name: name, ok: false, error: String(error.message || error)}); }
  }
  function simplifyLegacy() {
    var before = document.documentElement.outerHTML;
    var status = JSON.parse(new Function('return (\n' + testSources.simplify.trim().replace(/;$/, '') + '\n);')());
    assert(status.ok, 'legacy simplification failed');
    assert(document.documentElement.outerHTML === before, 'legacy simplifier mutated live DOM');
    return new DOMParser().parseFromString(window.__seimiSimplify.chunks.join(''), 'text/html');
  }
  test('original simplifier preserves configure calls and literal comment endings', function () {
    var code = 'configure({retries: 3});\n  next({url: "/api"});\n<!-- literal -->';
    page('<pre><code></code></pre><p><code>configure({retries: 3});</code></p><p id="prose">configure({retries: 3});</p><script>REAL_SCRIPT_NOISE</script><!-- REAL_COMMENT_NOISE -->');
    document.querySelector('pre code').textContent = code;
    var result = simplifyLegacy();
    assert(result.querySelector('pre code').textContent === code, 'code changed during legacy cleanup');
    assert(result.querySelector('p code').textContent === 'configure({retries: 3});', 'inline code deleted');
    assert(result.querySelector('#prose').textContent === 'configure({retries: 3});', 'code-like prose deleted');
    absent(result.body.textContent, 'REAL_SCRIPT_NOISE');
    absent(result.documentElement.outerHTML, 'REAL_COMMENT_NOISE');
  });
  test('original simplifier retains visible aria-hidden content', function () {
    page('<p aria-hidden="true">VISIBLE_ARIA</p><p hidden>HIDDEN_ATTR</p><p style="display:none">HIDDEN_DISPLAY</p><p style="visibility:hidden">HIDDEN_VISIBILITY</p>');
    var result = simplifyLegacy();
    contains(result.body.textContent, 'VISIBLE_ARIA');
    absent(result.body.textContent, 'HIDDEN_');
  });
  test('both converters receive intact code and visible aria-hidden content', function () {
    page('<pre><code>configure({retries: 3});</code></pre><p aria-hidden="true">VISIBLE</p>');
    ['turndown', 'legacy_cpp'].forEach(function (converter) {
      var result = convert({converter: converter});
      contains(result.text, 'configure({retries: 3});');
      contains(result.text, 'VISIBLE');
    });
  });
  test('ordered lists retain nested blockquotes and unordered lists', function () {
    page('<ol start="3"><li>Outer<blockquote><p>Quoted</p><ul><li>Nested</li></ul></blockquote><p>After quote</p></li><li>Next</li></ol>');
    var result = convert();
    contains(result.text, '3.  Outer');
    contains(result.text, '4.  Next');
    assert(/^ {4}> Quoted$/m.test(result.text), 'blockquote escaped its parent list: ' + result.text);
    assert(/^ {4}> - +Nested$/m.test(result.text), 'nested list escaped its blockquote: ' + result.text);
    assert(/^ {4}After quote$/m.test(result.text), 'continuation escaped its parent item: ' + result.text);
  });
  test('semantic DOM, exact attribute boundaries, base URL, entities and Unicode', function () {
    page('<h1>标题 &amp; café 😀</h1><p>literal &lt;script&gt; &amp;copy;</p><a data-href="/wrong" href="../real?q=1&amp;k=2">right</a><img alt="猫" data-src="images/cat.png">');
    var output = convert().text;
    contains(output, '# 标题 &amp; café 😀');
    contains(output, '&lt;script>');
    contains(output, '&amp;copy;');
    contains(output, '[right](https://example.com/real?q=1&k=2)');
    contains(output, '![猫](https://example.com/docs/images/cat.png)');
    absent(output, '/wrong');
  });
  test('rendered CSS visibility and accessible-hidden visible content', function () {
    page('<p class="gone">CSS_GONE</p><p hidden>ATTR_GONE</p><p hidden style="display:block">OVERRIDE_VISIBLE</p><p aria-hidden="true">VISIBLE_ARIA</p><div class="invisible">HIDDEN_TEXT<span style="visibility:visible">RESTORED</span><span>INHERITED_HIDDEN</span></div>', '.unused{}<style>.gone{display:none}.invisible{visibility:hidden}</style>');
    var output = convert().text;
    contains(output, 'VISIBLE\\_ARIA');
    contains(output, 'RESTORED');
    contains(output, 'OVERRIDE\\_VISIBLE');
    absent(output, 'GONE'); absent(output, 'HIDDEN');
  });
  test('code preserves literal scripts, indentation, blank lines, br and fence conflicts', function () {
    page('<div class="highlight-source-js"><pre><code>jsLoader({a:1});\n  &lt;script&gt;literal&lt;/script&gt;\n\n```\nend<br>line</code></pre></div><p><code> a  b ` c </code></p>');
    var output = convert().text;
    contains(output, '````js\njsLoader({a:1});\n  <script>literal</script>\n\n```\nend\nline\n````');
    contains(output, '``  a  b ` c  ``');
  });
  test('nested lists, ordered starts, live task state and strikethrough', function () {
    page('<ol start="3"><li>outer<ul><li>nested</li></ul></li><li><label><input type="checkbox">task</label></li></ol><p><del>removed</del></p>');
    document.querySelector('input').checked = true;
    var output = convert().text;
    contains(output, '3.  outer\n    -   nested');
    contains(output, '4.  [x] task');
    contains(output, '~~removed~~');
  });
  test('simple GFM tables: pipes, code, line breaks, alignment', function () {
    page('<table>\n<thead><tr><th align="left">A</th><th align="right">B</th></tr></thead>\n<tbody><tr><td>x|y<br>z</td><td><code>v|w</code></td></tr></tbody></table>');
    var result = convert();
    contains(result.text, '| A | B |\n| :--- | ---: |\n| x\\|y<br>z | `v\\|w` |');
    assert(result.status.warnings.length === 0, 'unexpected warning');
  });
  test('complex tables preserve spans and sanitize active HTML', function () {
    page('<table onclick="evil()"><caption>Data</caption><tbody><tr><td rowspan="2" style="color:red"><a href="javascript:evil()">unsafe</a><img src="data:image/svg+xml,bad" onerror="evil()">A</td><td>B</td></tr><tr><td><p>C</p><table><tr><td>nested</td></tr></table></td></tr></tbody></table>');
    var result = convert();
    contains(result.text, '<table>'); contains(result.text, 'rowspan="2"'); contains(result.text, '<caption>Data</caption>');
    contains(result.text, '<a>unsafe</a>'); contains(result.text, 'nested');
    absent(result.text, 'javascript:'); absent(result.text, 'onerror'); absent(result.text, 'onclick'); absent(result.text, 'style='); absent(result.text, 'image/svg');
    assert(result.status.warnings.includes('table_preserved_as_html'), 'missing HTML fallback warning');
    assert(result.status.warnings.includes('unsafe_url_removed'), 'missing unsafe URL warning');
  });
  test('empty and headerless tables do not crash or invent a header', function () {
    page('<table></table><table><tr><td>data only</td></tr></table>');
    var result = convert();
    assert(result.status.ok, 'conversion failed');
    contains(result.text, '<td>data only</td>'); absent(result.text, '| ---');
  });
  test('raw scripts and comments removed without deleting legitimate code-like prose', function () {
    page('<script>window.evil=1</script><!-- HIDDEN_COMMENT --><style>body{color:red}</style><p>jsLoader({a:1}); &lt;script&gt;displayed source&lt;/script&gt;</p>');
    var output = convert().text;
    absent(output, 'window.evil'); absent(output, 'HIDDEN_COMMENT'); absent(output, 'color:red');
    contains(output, 'jsLoader({a:1});'); contains(output, '&lt;script>displayed source&lt;/script>');
  });
  test('unsafe links rejected in both converter selections', function () {
    page('<p><a href="java&#x09;script:alert(1)">JS</a><a href="file:///tmp/local">FILE</a><a href="mailto:a@example.com">MAIL</a><a href="//cdn.example.com/path">CDN</a></p>');
    ['turndown', 'legacy_cpp'].forEach(function (converter) {
      var result = convert({converter: converter});
      assert(result.status.ok, 'conversion failed');
      absent(result.text, 'javascript:'); absent(result.text, 'file:');
      contains(result.text, 'mailto:a@example.com'); contains(result.text, 'https://cdn.example.com/path');
    });
  });
  test('long documents retain final attributes and do not split surrogate pairs', function () {
    page('<p>' + 'x'.repeat(29999) + '😀' + 'x'.repeat(45000) + '</p><a href="last">LAST_LINK</a>');
    var result = convert();
    assert(result.status.ok && result.status.totalChars > 65535, 'long document failed');
    contains(result.text, '😀'); contains(result.text, '(https://example.com/docs/last)');
    window.__seimiMarkdown.chunks.forEach(function (chunk) {
      assert(!/[\uD800-\uDBFF]$/.test(chunk), 'chunk ends with high surrogate');
      assert(!/^[\uDC00-\uDFFF]/.test(chunk), 'chunk starts with low surrogate');
    });
  });
  test('currentSrc is preferred over placeholder src and base URI is honored', function () {
    page('<img src="placeholder.png" alt="selected">');
    Object.defineProperty(document.querySelector('img'), 'currentSrc', {value: 'https://cdn.example.com/selected@2x.png'});
    var output = convert().text;
    contains(output, 'https://cdn.example.com/selected@2x.png'); absent(output, 'placeholder.png');
  });
  test('lazy image attributes replace tiny or placeholder sources', function () {
    page('<img src="spacer.gif" data-original-src="photos/real.png" alt="lazy"><img src="data:image/png;base64,AAAA" data-lazy-src="photos/second.webp">');
    var output = convert().text;
    contains(output, 'https://example.com/docs/photos/real.png');
    contains(output, 'https://example.com/docs/photos/second.webp');
    absent(output, 'spacer.gif'); absent(output, 'base64,AAAA');
  });
  test('HTML table fallback encodes blank lines while preserving code text', function () {
    page('<table><tr><td><pre>one\n\ntwo &lt; three</pre></td></tr></table>');
    var result = convert();
    contains(result.text, '<pre>one&#10;&#10;two &lt; three</pre>');
    absent(result.text, '\n\n');
    var roundtrip = new DOMParser().parseFromString(result.text, 'text/html');
    assert(roundtrip.querySelector('pre').textContent === 'one\n\ntwo < three', 'HTML code changed');
  });
  test('Readability code/list pages retain useful structures or report fallback', function () {
    [
      '<pre><code class="language-js">' + 'call({answer:42});\n'.repeat(30) + '</code></pre>',
      '<ul><li>First meaningful list item.</li><li>Second meaningful list item.</li></ul>',
      '<table><tr><th>Property</th><th>Value</th></tr><tr><td>enabled</td><td>true</td></tr></table>'
    ].forEach(function (fixture) {
      page(fixture);
      var result = convert({algorithm: 'readability'});
      assert(result.status.ok, 'structured extraction failed');
      if (fixture.includes('<pre>')) contains(result.text, '```js\n' + 'call({answer:42});\n'.repeat(30));
      else if (fixture.includes('<ul>')) contains(result.text, '-   First meaningful list item.');
      else contains(result.text, '| enabled | true |');
      if (result.status.extractor === 'conservative') assert(result.status.fallback_reason, 'fallback lacked a reason');
    });
  });
  test('Readability structure-loss guard reports deliberate conservative fallback', function () {
    page('<ul><li>First useful item</li><li>Second useful item</li></ul>');
    var original = testSources.readability;
    try {
      testSources.readability = 'function Readability(doc) { this.parse = function() { var node = doc.createElement("div"); node.textContent = "First useful item"; return {content:node,textContent:node.textContent}; }; }';
      var result = convert({algorithm: 'readability'});
      assert(result.status.extractor === 'conservative', 'loss guard failed');
      assert(result.status.fallback_reason === 'readability_lost_structured_content', 'loss reason absent');
      contains(result.text, '-   Second useful item');
    } finally { testSources.readability = original; }
  });
  test('empty pre language and visible graphic labels are retained', function () {
    page('<pre><code class="language-python">\n\n</code></pre><svg aria-hidden="true"><title>Flow diagram</title><path d="M0 0"></path></svg><canvas>Canvas fallback</canvas>');
    var result = convert();
    contains(result.text, '```python\n\n\n```'); contains(result.text, 'Flow diagram'); contains(result.text, 'Canvas fallback');
    assert(result.status.warnings.includes('graphic_text_only'), 'graphic fallback not reported');
  });
  test('Readability returns DOM and retains fenced code language', function () {
    page('<nav>Navigation</nav><article><h1>Article title</h1><p>' + 'This is a substantial article, with useful content and sentences. '.repeat(30) + '</p><pre><code class="language-cpp">if (a &lt; b) {\n  f({x:1});\n}</code></pre><p>' + 'More detailed paragraphs for readers. '.repeat(25) + '</p></article>');
    var result = convert({algorithm: 'readability'});
    assert(result.status.ok && result.status.extractor === 'readability', 'Readability did not succeed: ' + JSON.stringify(result.status));
    contains(result.text, '```cpp\nif (a < b) {\n  f({x:1});\n}\n```');
  });
  test('Readability absence reports conservative fallback', function () {
    page('<p>Keep this content</p>');
    var result = convert({algorithm: 'readability'}, true);
    assert(result.status.ok && result.status.extractor === 'conservative', 'fallback did not succeed');
    assert(result.status.fallback_reason === 'readability_unavailable', 'fallback reason absent');
    contains(result.text, 'Keep this content');
  });
  test('invalid selection clears stale chunks and returns an explicit error', function () {
    page('<p>First</p>'); convert();
    var result = convert({converter: 'invalid'});
    assert(!result.status.ok && result.text === '', 'failure retained old data');
    assert(result.status.error === 'invalid_markdown_converter', 'missing error');
  });
  test('payloads larger than 32 Mi UTF-16 units fail explicitly', function () {
    page('<p>' + 'x'.repeat(32 * 1024 * 1024) + '</p>');
    var result = convert({converter: 'legacy_cpp'});
    assert(!result.status.ok && result.text === '', 'oversized payload was returned');
    assert(result.status.error === 'markdown_payload_too_large', 'size failure reason absent');
  });
  document.head.innerHTML = '<meta charset="utf-8">';
  document.body.innerHTML = '<pre id="test-results"></pre>';
  document.querySelector('pre').textContent = JSON.stringify(results);
})();
