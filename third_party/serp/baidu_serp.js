// seimi-render 百度搜索结果页（SERP）结构化提取脚本。
// 由 RenderWorker::extractSerp() 在 GUI 线程注入到已加载的百度结果页 DOM 上运行。
// 选择器基于 2026-07 真实渲染校准（见 docs/superpowers/specs/2026-07-01-baidu-serp-extraction-design.md）。
// 协议与 extract.js/simplify.js 同构：结果存 window.__seimiSerp.chunks（30k 分块），
// 返回值只含小状态 JSON {ok,totalChunks}，规避 runJavaScript 大返回值截断。
(function () {
  function extractBaiduSerp() {
    var out = { engine: "baidu", blocked: false, results: [], recommend: [], meta: {} };

    // 1. 反爬拦截页检测：title 含"安全验证"或 body 文本极短（实测拦截页仅 ~1.5KB）。
    var t = document.title || "";
    var bodyText = (document.body && document.body.innerText) || "";
    if (/安全验证/.test(t) || bodyText.length < 50) {
      out.blocked = true;
      return stash(out);
    }

    // 2. 限定主结果区（实测所有结果都在 #content_left 内）。
    var scope = document.querySelector('#content_left') || document.body;

    // 3. 收集所有 c-container 候选（自然结果 + 百度卡片 + 广告都在内）。
    var containers = scope.querySelectorAll('div.c-container');
    var seenTitles = {};  // 嵌套去重：广告 div 内嵌套同标题 div
    var adCount = 0;

    containers.forEach(function (c) {
      var cls = c.className || "";
      var mu = c.getAttribute("mu") || "";
      var tpl = c.getAttribute("tpl") || "";

      // 3a. 广告判定（实测可靠信号）：
      //   - 容器 class 含 EC_result / ec-pc-huanxin-grid / ec-pc-fresh-font-color
      //   - 子元素有 <span class="ec-tuiguang"> 角标
      //   实测：tuiguang 不在容器 class 而在子 span；整页无"广告"字面文本。
      var isAd = /EC_result|ec-pc-huanxin|ec-pc-fresh-font-color/.test(cls) ||
                 !!c.querySelector('span[class*="ec-tuiguang"]');
      if (isAd) { adCount++; return; }

      // 3b. "其他人还在搜"聚合剔除。
      if (tpl === "recommend_list") return;

      // 3c. 标题（容器内 h3 > a 或 h3）。
      var h3a = c.querySelector('h3 a') || c.querySelector('h3');
      if (!h3a) return;
      var title = (h3a.innerText || h3a.textContent || "").trim();
      if (!title || seenTitles[title]) return;  // 嵌套去重
      seenTitles[title] = true;

      // 3d. URL：mu 优先（实测自然结果 mu=真实URL，如 python.org）。
      //     mu 缺失或为 baidu.com/link?/nourl.ubs → 跳转链，标 is_redirect。
      var href = (h3a.getAttribute && h3a.getAttribute("href")) || "";
      var url = mu || href;
      var isRedirect = !mu || /baidu\.com\/link\?url=/.test(url) || /nourl\.ubs\.baidu\.com/.test(url);

      // 3e. 摘要（实测 summary-text_560AW，class 末尾常带换行空格，用前缀通配）。
      var desc = "";
      var descEl = c.querySelector('span[class*="summary-text"], .c-abstract, span[class*="content-right"]');
      if (descEl) desc = (descEl.innerText || "").trim();

      // 3f. 来源（实测 cosc-source-text 精确命中）。
      var source = "";
      var srcEl = c.querySelector('.cosc-source-text, .c-showurl');
      if (srcEl) source = (srcEl.innerText || "").trim();

      // 3g. 类型：result-op 为百度聚合卡（百科/翻译/贴吧等）。
      var isCard = /result-op/.test(cls);

      out.results.push({
        title: title,
        url: url,
        is_redirect: isRedirect,
        snippet: desc,
        source: source,
        type: isCard ? "baidu_card" : "organic"
      });
    });

    // 4. 相关搜索（#rs 区，单独入 recommend，不混入 results）。
    var rs = document.querySelector('#rs');
    if (rs) rs.querySelectorAll('a').forEach(function (a) {
      var tx = (a.innerText || "").trim();
      if (tx) out.recommend.push(tx);
    });

    out.meta = { count: out.results.length, ads_filtered: adCount };
    return stash(out);
  }

  // 把结果对象分块存入 window，返回小状态 JSON（与 extract.js/simplify.js 同协议）。
  function stash(obj) {
    var s = JSON.stringify(obj);
    var CHUNK = 30000, chunks = [];
    for (var i = 0; i < s.length; i += CHUNK) chunks.push(s.substring(i, i + CHUNK));
    window.__seimiSerp = { chunks: chunks };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  }

  try {
    return extractBaiduSerp();
  } catch (e) {
    window.__seimiSerp = { chunks: [""] };
    return JSON.stringify({ ok: false, error: "" + e });
  }
})();
