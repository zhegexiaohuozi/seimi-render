/*
 * seimi-render 的 conservative-markdown DOM 简化器（轻量版，不依赖 Readability）。
 *
 * 目的：conservative 是默认 markdown 算法。原先直接把完整 DOM 喂给 html2md，但
 * html2md 是字符串级解析器，遇到 script 内的 '<'（如 a<b、正则 /</g、字符串 '<div>'）
 * 会状态错乱，导致 JS 源码泄漏进 markdown（新浪等大站尤其严重）。
 *
 * 这里在 document 的深拷贝上做轻度清理，把简化后的 HTML 字符串回传给 C++，
 * 再交给 html2md。这样 html2md 拿到的 HTML 已经无 script/style 等噪音，解析压力骤降。
 *
 * 轻度策略（零正文误伤）：
 *   - 删：script / style / noscript / template / iframe / link
 *   - 删：非 viewport 的 meta（保留 charset/viewport/content-type/x-ua-compatible）
 *   - 删：aria-hidden="true" 的元素、内联 style 含 display:none/visibility:hidden 的元素
 *   - 保留：nav / aside / footer / header / form（HTML5 允许它们嵌在 article 里承载正文）
 *
 * 关键：只动 document.cloneNode(true) 的克隆，绝不动 live document。
 * 原始 DOM 还要用于 html 输出、PDF 打印、像素截图（三者并发读 live page，任何修改都会污染）。
 *
 * 调用约定（被 RenderPool::extractSimplifiedHtml 注入）：
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

    // 3) 隐藏元素清理。aria-hidden="true" 是 WAI-ARIA 明确的"对用户不可见"语义；
    //    内联 display:none/visibility:hidden 同样是显式隐藏。这些内容本就不该进 markdown。
    //    注意：visibility:hidden 的子元素若显式 visibility:visible 会重新可见，
    //    但这种写法极少见且几乎不会承载正文，简化起见整体删除。
    clone.querySelectorAll('[aria-hidden="true"]').forEach(function(el){ el.remove(); });
    clone.querySelectorAll('*').forEach(function(el){
      var s = el.getAttribute('style');
      if (s && /display\s*:\s*none|visibility\s*:\s*hidden/i.test(s)) el.remove();
    });

    // 4) 序列化克隆。outerHTML 已含 <html>...</html>，外层包一层保证独立可解析。
    var html = '<html>' + clone.documentElement.outerHTML + '</html>';

    // 4a) 字符串层兜底清理：处理 DOM 层删不掉的残留。
    //     (a) HTML 注释残留：outerHTML 会把 Comment 节点序列化回 <!--...-->；
    //         若上一步 TreeWalker 因克隆差异漏删（极少见），这里再保险一次。
    //         条件注释（[if IE]>...<![endif]）也一并删。
    //     (b) 残留的注释闭合尾巴 "-->"：老式写法 <!-- ... //--> 在前面删注释时
    //         若开头的 <!-- 在另一个分块里被截断，可能留下孤立的 "-->"，
    //         单独出现的 --> 对正文毫无意义，直接删。
    //     (c) 裸 JS 调用残留：如 jsLoader({name:'adNone',url:'...'}); 这类没被
    //         <script> 也没被 <!-- --> 包裹、作为文本节点散落在 body 里的脚本调用。
    //         【关键】只删"标识符({...});"模式——括号内必须含 { 才视为对象字面量
    //         调用（jsLoader/require/define 等），这样不会误伤正文里 "函数 f(x)
    //         的值"、"sin(30°)"、"ECMAScript 规范" 这类正常文本（它们的括号里是
    //         变量/表达式而非对象）。document.write("...") 这类字符串参数的调用
    //         不在此列，但它们几乎都在 <script> 内已被第 1 步删除。
    html = html.replace(/<!--[\s\S]*?-->/g, '');              // (a) HTML 注释（含条件注释）
    html = html.replace(/-->/g, '');                          // (b) 残留注释闭合符
    html = html.replace(/\b[a-zA-Z_$][\w$]*\s*\(\s*\{[^{}]*\}\s*\)\s*;?\s*/g, '');
    // ↑ (c) 只删"标识符({...});"：括号内必须是单个 {...} 对象字面量。
    //      不匹配嵌套对象（jsLoader({a:{b:1}}) 之类），因为那种复杂结构在正文文本
    //      节点里几乎不存在，宁可漏删也不冒险误伤。

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
    // 任何异常（克隆失败/选择器异常）都视为失败，C++ 回退到原始 DOM（等价今天的 conservative）。
    window.__seimiSimplify = { status: 'error', reason: String(e && e.message ? e.message : e) };
    return JSON.stringify({ ok: false });
  }
}

return simplify();

})();
