/*
 * seimi-render 旧版 DOM 简化器（保留作兼容基线，不依赖 Readability）。
 * 当前两种转换器共用 third_party/markdown/convert.js；本文件由直接回归测试覆盖。
 *
 * 目的：conservative 是默认 markdown 算法。原先直接把完整 DOM 喂给 html2md，但
 * html2md 是字符串级解析器，遇到 script 内的 '<'（如 a<b、正则 /</g、字符串 '<div>'）
 * 会状态错乱，导致 JS 源码泄漏进 markdown（新浪等大站尤其严重）。
 *
 * 这里在 document 的深拷贝上做轻度清理，把简化后的 HTML 字符串回传给 C++，
 * 再交给 html2md。这样 html2md 拿到的 HTML 已经无 script/style 等噪音，解析压力骤降。
 *
 * 轻度策略：
 *   - 删：script / style / noscript / template / iframe / link
 *   - 删：非 viewport 的 meta（保留 charset/viewport/content-type/x-ua-compatible）
 *   - 删：hidden 的元素、内联 style 含 display:none/visibility:hidden 的元素
 *   - 保留：nav / aside / footer / header / form（HTML5 允许它们嵌在 article 里承载正文）
 *
 * 关键：只动 document.cloneNode(true) 的克隆，绝不动 live document。
 * 原始 DOM 还要用于 html 输出、PDF 打印、像素截图（三者并发读 live page，任何修改都会污染）。
 *
 * 独立调用约定：
 *   runJavaScript(整段文件) → 返回小状态 JSON { ok, totalChunks }
 *   大正文按 30k 分块存 window.__seimiSimplify.chunks[]，C++ 逐块读取拼接。
 *   （与 extract.js 的 chunk 协议完全一致，规避 runJavaScript 大返回值截断。）
 */
(function(){

function simplify() {
  try {
    // 深拷贝，避免污染 live DOM（toHtml/printToPdf/grab 并发读 live page）。
    var clone = document.cloneNode(true);

    // 1) 删除噪音标签：脚本/样式/模板/iframe/link 全部不含正文，删之零误伤。
    //    noscript 内容是给禁 JS 环境看的替代文案，渲染场景下也是噪音。
    clone.querySelectorAll('script, style, noscript, template, iframe, link')
         .forEach(function(el){ el.remove(); });

    // 1a) 删除 HTML 注释节点（Node.COMMENT_NODE）。
    //     老式页面（尤其新浪等门户）习惯把统计/广告 JS 包在 <!-- ... //--> 或 <!-- ... --> 里，
    //     如 <!-- jsLoader({name:'adNone',url:'//d1.sina.com.cn/.../adNone.js'}); //-->
    //     删 <script> 只能干掉标签内的，注释里的 JS 调用会作为文本残留进 markdown。
    //     直接用 TreeWalker 删 Comment 节点最干净，正文里几乎不会有合法注释。
    (function stripComments(root){
      var walker = document.createTreeWalker(
        root, NodeFilter.SHOW_COMMENT, null);
      var toRemove = [];
      while (walker.nextNode()) toRemove.push(walker.currentNode);
      for (var i = 0; i < toRemove.length; ++i) toRemove[i].parentNode.removeChild(toRemove[i]);
    })(clone);

    // 2) meta 清理：保留 charset/viewport/content-type/x-ua-compatible（影响渲染/编码），
    //    删 SEO 类（description/keywords/referrer 等）—— 它们对 markdown 输出无意义。
    clone.querySelectorAll('meta').forEach(function(m){
      var n = (m.getAttribute('name') || '').toLowerCase();
      var p = (m.getAttribute('http-equiv') || '').toLowerCase();
      var eq = (m.getAttribute('charset') || '').toLowerCase();
      var preserve = (eq !== '') || (n === 'viewport') ||
                     (p === 'content-type') || (p === 'x-ua-compatible');
      if (!preserve) m.remove();
    });

    // 3) 仅移除明确不可见节点；aria-hidden 是辅助技术语义，不表示视觉隐藏。
    clone.querySelectorAll('[hidden]').forEach(function(el){ el.remove(); });
    clone.querySelectorAll('*').forEach(function(el){
      if (el.style.display === 'none' || el.style.visibility === 'hidden') el.remove();
    });

    // 4) 保留正文文本原样。全局“疑似 JS”正则会误删 pre/code 和技术文档示例。
    var html = clone.documentElement.outerHTML;

    // 5) 分块存 window（CHUNK=30000 远小于 runJavaScript 返回值上限，不截断）。
    var CHUNK = 30000;
    var chunks = [];
    for (var i = 0; i < html.length; i += CHUNK) {
      chunks.push(html.substring(i, i + CHUNK));
    }
    window.__seimiSimplify = {
      status: 'ok',
      totalChunks: chunks.length,
      chunks: chunks
    };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  } catch (e) {
    // 克隆/清理失败时返回明确的失败状态，由调用方处理。
    window.__seimiSimplify = { status: 'error', reason: String(e && e.message ? e.message : e) };
    return JSON.stringify({ ok: false });
  }
}

return simplify();

})();
