(function () {
/*
 * SPDX-License-Identifier: Apache-2.0
 * Browser-only DOM -> Markdown adapter for seimi-render.
 * Evaluated after Turndown, its GFM plugins and (optionally) Readability in
 * QWebEngineScript::ApplicationWorld. Never writes to the live document.
 * Input: window.__seimiMarkdownOptions = { algorithm, converter }.
 * Output: small JSON status; payload in window.__seimiMarkdown.chunks.
 */
  'use strict';

  var started = performance.now();
  var options = window.__seimiMarkdownOptions || {};
  var converter = options.converter || 'turndown';
  var extractor = options.algorithm || 'conservative';
  var warnings = [];
  var fallbackReason = '';
  var title = document.title || '';
  var MAX_CHARS = 32 * 1024 * 1024;
  window.__seimiMarkdown = { chunks: [] };

  function warn(message) {
    if (warnings.indexOf(message) < 0) warnings.push(message);
  }

  function safeUrl(value, base, image) {
    if (!value || !value.trim()) return '';
    try {
      var url = new URL(value, base);
      if (/^https?:$/.test(url.protocol)) return url.href;
      if (!image && /^(mailto|tel|ftp):$/.test(url.protocol)) return url.href;
      if (image && /^data:image\/(png|jpeg|gif|webp|avif);base64,[a-z0-9+/=\s]+$/i.test(value)) return value;
      warn('unsafe_url_removed');
    } catch (error) {
      warn('invalid_url_removed');
    }
    return '';
  }

  function languageFor(node) {
    for (var el = node, depth = 0; el && depth < 4; el = el.parentElement, ++depth) {
      var language = el.getAttribute('data-language') || el.getAttribute('data-lang') || '';
      var classes = el.getAttribute('class') || '';
      if (!language) language = (classes.match(/(?:^|\s)(?:language-|lang-|highlight-(?:source|text)-)([\w#+.-]+)/) || [])[1] || '';
      if (/^[\w#+.-]{1,40}$/.test(language)) return language;
    }
    return '';
  }

  function selectedImageUrl(image) {
    var current = image.currentSrc || image.getAttribute('src') || '';
    var lazy = image.getAttribute('data-src') || image.getAttribute('data-original') ||
      image.getAttribute('data-original-src') || image.getAttribute('data-lazy-src') || image.getAttribute('lazy-src');
    var tiny = image.complete && image.naturalWidth > 0 && image.naturalWidth <= 2 && image.naturalHeight <= 2;
    var placeholder = /(?:^|[\/_.-])(placeholder|spacer|blank|transparent|loading)(?:[\/_.-]|$)/i.test(current);
    return lazy && (!current || /^data:image\//i.test(current) || tiny || placeholder) ? lazy : current;
  }

  function fencedCode(node) {
    var code = node.textContent.replace(/\r\n?/g, '\n');
    var runs = code.match(/`+/g) || [];
    var longest = runs.reduce(function (max, run) { return Math.max(max, run.length); }, 2);
    var fence = '`'.repeat(longest + 1);
    var language = node.getAttribute('data-seimi-language') || languageFor(node.querySelector('code') || node);
    return '\n\n' + fence + language + '\n' + code + (code.endsWith('\n') ? '' : '\n') + fence + '\n\n';
  }

  function normalizedDocument() {
    var clone = document.cloneNode(true);
    var liveNodes = document.querySelectorAll('*');
    var clonedNodes = clone.querySelectorAll('*');
    // Read rendered state before removing style sheets or altering clone structure.
    for (var i = 0; i < liveNodes.length; ++i) {
      var live = liveNodes[i];
      var copy = clonedNodes[i];
      copy.removeAttribute('data-seimi-hidden');
      copy.removeAttribute('data-seimi-language');
      var style = window.getComputedStyle(live);
      if (style.display === 'none' || style.contentVisibility === 'hidden') copy.setAttribute('data-seimi-hidden', 'subtree');
      else if (style.visibility === 'hidden' || style.visibility === 'collapse') copy.setAttribute('data-seimi-hidden', 'text');
      // aria-hidden describes accessibility, not visual visibility. Readability
      // must not discard visible icons, formulas or code marked aria-hidden.
      copy.removeAttribute('aria-hidden');
      // CSS may override the hidden attribute; rendered state above is decisive.
      copy.removeAttribute('hidden');
      copy.removeAttribute('style');
      if (/^(TH|TD)$/.test(copy.nodeName) && /^(left|right|center)$/.test(style.textAlign)) copy.setAttribute('align', style.textAlign);
      if (copy.nodeName === 'A') {
        var href = safeUrl(live.getAttribute('href'), live.baseURI, false);
        if (href) copy.setAttribute('href', href);
        else copy.removeAttribute('href');
      }
      if (copy.nodeName === 'IMG') {
        var candidate = selectedImageUrl(live);
        var src = safeUrl(candidate, live.baseURI, true);
        if (src) copy.setAttribute('src', src);
        else copy.removeAttribute('src');
        // Prevent Readability's lazy image heuristic from restoring rejected URLs.
        ['srcset', 'data-src', 'data-original', 'data-original-src', 'data-lazy-src', 'lazy-src', 'data-srcset'].forEach(function (attr) { copy.removeAttribute(attr); });
      }
      if (copy.nodeName === 'INPUT' && live.type === 'checkbox') {
        copy.checked = live.checked;
        if (live.checked) copy.setAttribute('checked', '');
        else copy.removeAttribute('checked');
      }
      if (copy.nodeName === 'PRE') {
        var language = languageFor(live.querySelector('code') || live);
        if (language) copy.setAttribute('data-seimi-language', language);
      }
      // HTML passthrough is sanitized separately; stripping active attributes
      // here also makes the legacy converter input safe to inspect.
      Array.from(copy.attributes).forEach(function (attr) {
        if (/^on/i.test(attr.name) || attr.name === 'srcdoc') copy.removeAttribute(attr.name);
      });
    }
    clone.querySelectorAll('[data-seimi-hidden="subtree"]').forEach(function (node) { node.remove(); });
    clone.querySelectorAll('[data-seimi-hidden="text"]').forEach(function (node) {
      // visibility can be restored on a descendant; retain those descendants.
      Array.from(node.childNodes).forEach(function (child) {
        if (child.nodeType === 3) child.remove();
      });
      if (/^(IMG|INPUT|BR|HR)$/.test(node.nodeName)) node.remove();
    });
    clone.querySelectorAll('[data-seimi-hidden]').forEach(function (node) { node.removeAttribute('data-seimi-hidden'); });
    clone.querySelectorAll('script,style,noscript,template,iframe,object,embed,link,meta,base').forEach(function (node) { node.remove(); });
    clone.querySelectorAll('svg,canvas').forEach(function (node) {
      var label = node.getAttribute('aria-label') || (node.querySelector('title') || {}).textContent || '';
      if (!label) label = node.nodeName.toLowerCase() === 'canvas' ? node.textContent :
        Array.from(node.querySelectorAll('text')).map(function (text) { return text.textContent; }).join(' ');
      node.replaceWith(clone.createTextNode(label));
      warn(label ? 'graphic_text_only' : 'graphic_omitted');
    });
    var walker = clone.createTreeWalker(clone, NodeFilter.SHOW_COMMENT);
    var comments = [];
    while (walker.nextNode()) comments.push(walker.currentNode);
    comments.forEach(function (node) { node.remove(); });
    // Highlight libraries commonly express line breaks as <br> inside <pre>.
    clone.querySelectorAll('pre br').forEach(function (node) { node.replaceWith(clone.createTextNode('\n')); });
    return clone;
  }

  function sanitizeTable(table) {
    var clone = table.cloneNode(true);
    var allowed = /^(TABLE|CAPTION|COLGROUP|COL|THEAD|TBODY|TFOOT|TR|TH|TD|P|DIV|SPAN|BR|HR|A|IMG|STRONG|B|EM|I|DEL|S|STRIKE|CODE|PRE|UL|OL|LI|BLOCKQUOTE|SUP|SUB|H[1-6])$/;
    Array.from(clone.querySelectorAll('*')).forEach(function (node) {
      if (!allowed.test(node.nodeName)) node.replaceWith.apply(node, Array.from(node.childNodes));
    });
    [clone].concat(Array.from(clone.querySelectorAll('*'))).forEach(function (node) {
      Array.from(node.attributes).forEach(function (attr) {
        var keep = (attr.name === 'href' && node.nodeName === 'A') ||
          (attr.name === 'src' && node.nodeName === 'IMG') ||
          (/^(alt|title)$/.test(attr.name)) ||
          (/^(colspan|rowspan)$/.test(attr.name) && /^(TH|TD)$/.test(node.nodeName) && /^\d+$/.test(attr.value)) ||
          (attr.name === 'align' && /^(left|right|center)$/.test(attr.value)) ||
          (attr.name === 'start' && node.nodeName === 'OL' && /^\d+$/.test(attr.value));
        if (!keep) node.removeAttribute(attr.name);
      });
      ['href', 'src'].forEach(function (attr) {
        if (!node.hasAttribute(attr)) return;
        var value = safeUrl(node.getAttribute(attr), document.baseURI, attr === 'src');
        if (value) node.setAttribute(attr, value);
        else node.removeAttribute(attr);
      });
    });
    // CommonMark ends an HTML table block at a blank line. Character references
    // retain literal newlines (including <pre> blank lines) within one HTML line.
    return clone.outerHTML.replace(/\r\n?|\n/g, '&#10;');
  }

  function simpleTable(table) {
    var rows = Array.from(table.rows);
    if (!rows.length || !rows[0].cells.length) return false;
    if (table.querySelector('table,caption,pre,ul,ol,blockquote,p,div')) return false;
    if (!Array.from(rows[0].cells).every(function (cell) { return cell.nodeName === 'TH'; })) return false;
    var width = rows[0].cells.length;
    return rows.every(function (row, index) {
      return row.cells.length === width && Array.from(row.cells).every(function (cell) {
        return cell.colSpan === 1 && cell.rowSpan === 1 && (index === 0 || cell.nodeName === 'TD');
      });
    });
  }

  function markdown(root) {
    if (typeof TurndownService !== 'function' || typeof turndownPluginGfm === 'undefined') throw new Error('markdown_converter_unavailable');
    var service = new TurndownService({
      headingStyle: 'atx', codeBlockStyle: 'fenced', bulletListMarker: '-',
      emDelimiter: '*', strongDelimiter: '**', preformattedCode: true,
      blankReplacement: function (content, node) {
        return node.nodeName === 'PRE' ? fencedCode(node) : (node.isBlock ? '\n\n' : '');
      }
    });
    // GFM's upstream table plugin assumes regular header rows and loses spans.
    // Our table rule below handles these explicitly and sanitizes HTML fallback.
    service.use([turndownPluginGfm.strikethrough, turndownPluginGfm.taskListItems]);
    var escape = service.escape;
    service.escape = function (text) {
      return escape.call(this, text.replace(/&/g, '&amp;').replace(/</g, '&lt;'));
    };
    service.addRule('seimiStrikethrough', {
      filter: ['del', 's', 'strike'],
      replacement: function (content) { return content ? '~~' + content + '~~' : ''; }
    });
    service.addRule('seimiTaskCheckbox', {
      filter: function (node) { return node.nodeName === 'INPUT' && node.type === 'checkbox' && node.closest('li'); },
      replacement: function (content, node) { return (node.checked ? '[x]' : '[ ]') + ' '; }
    });
    service.addRule('seimiCode', {
      filter: 'pre',
      replacement: function (content, node) { return fencedCode(node); }
    });
    service.addRule('seimiTable', {
      filter: 'table',
      replacement: function (content, node) {
        if (!simpleTable(node)) {
          warn('table_preserved_as_html');
          return '\n\n' + sanitizeTable(node) + '\n\n';
        }
        var rows = Array.from(node.rows);
        var lines = rows.map(function (row) {
          return '| ' + Array.from(row.cells).map(function (cell) {
            return service.turndown(cell).trim().replace(/\|/g, '\\|').replace(/\s*\n\s*/g, '<br>');
          }).join(' | ') + ' |';
        });
        var borders = Array.from(rows[0].cells).map(function (cell) {
          return ({ left: ':---', right: '---:', center: ':---:' })[cell.getAttribute('align')] || '---';
        });
        lines.splice(1, 0, '| ' + borders.join(' | ') + ' |');
        return '\n\n' + lines.join('\n') + '\n\n';
      }
    });
    return service.turndown(root);
  }

  function status(ok, extra) {
    return JSON.stringify(Object.assign({
      ok: ok, totalChunks: window.__seimiMarkdown.chunks.length, totalChars: 0,
      title: title, extractor: extractor, converter: converter, warnings: warnings,
      fallback_reason: fallbackReason, conversion_ms: Math.round((performance.now() - started) * 100) / 100
    }, extra || {}));
  }

  try {
    if (converter !== 'turndown' && converter !== 'legacy_cpp') throw new Error('invalid_markdown_converter');
    if (extractor !== 'conservative' && extractor !== 'readability') throw new Error('invalid_markdown_algorithm');
    var normalized = normalizedDocument();
    var root = normalized.body || normalized.documentElement;
    if (extractor === 'readability') {
      try {
        if (typeof Readability !== 'function') throw new Error('readability_unavailable');
        var article = new Readability(normalized.cloneNode(true), {
          charThreshold: 200, keepClasses: true, serializer: function (node) { return node; }
        }).parse();
        if (!article || !article.content || !article.textContent.trim()) throw new Error('readability_no_content');
        // Readability targets prose; code/list/table dominated pages may be
        // flattened or partly discarded. Prefer conservative extraction there.
        var textOf = function (node) { return node.textContent.replace(/\s+/g, ' ').trim(); };
        var structures = Array.from(root.querySelectorAll('pre,table,ul,ol')).filter(function (node) {
          return !node.parentElement.closest('pre,table,ul,ol') && textOf(node);
        });
        var structuredLength = structures.reduce(function (sum, node) { return sum + textOf(node).length; }, 0);
        if (structuredLength >= textOf(root).length / 2 && structures.some(function (node) {
          var selected = Array.from(article.content.querySelectorAll(node.nodeName.toLowerCase()));
          return !selected.some(function (candidate) { return textOf(candidate).includes(textOf(node)); });
        })) throw new Error('readability_lost_structured_content');
        root = article.content;
        title = article.title || title;
      } catch (error) {
        extractor = 'conservative';
        fallbackReason = String(error.message || error);
        warn('readability_fallback_to_conservative');
      }
    }
    // Readability may infer lazy image URLs from arbitrary data attributes.
    root.querySelectorAll('a[href],img[src]').forEach(function (node) {
      var attr = node.nodeName === 'A' ? 'href' : 'src';
      var value = safeUrl(node.getAttribute(attr), document.baseURI, attr === 'src');
      if (value) node.setAttribute(attr, value);
      else node.removeAttribute(attr);
    });
    var output = converter === 'legacy_cpp' ? root.outerHTML : markdown(root);
    if (output.length > MAX_CHARS) throw new Error('markdown_payload_too_large');
    var chunks = window.__seimiMarkdown.chunks;
    for (var start = 0; start < output.length;) {
      var end = Math.min(start + 30000, output.length);
      if (end < output.length && /[\uD800-\uDBFF]/.test(output.charAt(end - 1))) --end;
      chunks.push(output.substring(start, end));
      start = end;
    }
    return status(true, { totalChars: output.length });
  } catch (error) {
    window.__seimiMarkdown.chunks = [];
    return status(false, { error: String(error.message || error) });
  }
})();
