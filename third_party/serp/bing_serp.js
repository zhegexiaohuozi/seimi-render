// seimi-render 必应（Bing）搜索结果页（SERP）结构化提取脚本。
// 由 RenderWorker::extractSerp() 在 GUI 线程注入到已加载的 Bing 结果页 DOM 上运行。
// 选择器基于 2026-07 真实渲染校准（与 baidu_serp.js 同构，复用 chunk 协议）。
// Bing 结构比百度简单：li.b_algo 是自然结果，h2>a href 即真实 URL（无跳转链）。
(function () {
  function extractBingSerp() {
    var out = { engine: "bing", blocked: false, results: [], recommend: [], meta: {} };

    // 1. 反爬/异常检测：Bing 异常页 body 文本极短。
    var bodyText = (document.body && document.body.innerText) || "";
    if (bodyText.length < 50 || /consent\.bing\.com|verify you are human/i.test(document.title || "")) {
      out.blocked = true;
      return stash(out);
    }

    // 2. 限定主结果区（#b_results）。
    var scope = document.querySelector('#b_results') || document.body;

    // 3. 收集所有 li.b_algo 自然结果。广告容器是 li.sb_add（与自然结果同层级，按 class 区分）。
    var algos = scope.querySelectorAll('li.b_algo');
    var adCount = 0;
    // 统计广告（sb_add）用于可观测性，不混入 results。
    var ads = scope.querySelectorAll('li.sb_add, li.b_ad');
    adCount = ads.length;

    algos.forEach(function (li) {
      // 3a. 标题与链接（h2 > a，href 即真实 URL，Bing 不做跳转代理）。
      var h2a = li.querySelector('h2 a');
      if (!h2a) return;
      var title = (h2a.innerText || h2a.textContent || "").trim();
      if (!title) return;
      var url = h2a.getAttribute("href") || "";
      // Bing 的 href 总是真实目标 URL（https://...），无需去跳转。
      var isRedirect = false;

      // 3b. 摘要（b_caption 内的 p，或 b_lineclamp* 文本）。
      var desc = "";
      var descEl = li.querySelector('.b_caption p, p[class*="b_lineclamp"], .b_lineclamp2, .b_lineclamp4');
      if (descEl) desc = (descEl.innerText || "").trim();

      // 3c. 来源（cite 显示的 URL/域名，可能带面包屑 ›）。
      var source = "";
      var srcEl = li.querySelector('cite, .tptt, .b_attribution cite');
      if (srcEl) source = (srcEl.innerText || "").trim();

      out.results.push({
        title: title,
        url: url,
        is_redirect: isRedirect,
        snippet: desc,
        source: source,
        type: "organic"
      });
    });

    // 4. 相关搜索（b_rs 区）。
    var rs = scope.querySelector('.b_rs');
    if (!rs) rs = document.querySelector('.b_rs');
    if (rs) rs.querySelectorAll('a').forEach(function (a) {
      var tx = (a.innerText || "").trim();
      if (tx) out.recommend.push(tx);
    });

    out.meta = { count: out.results.length, ads_filtered: adCount };
    return stash(out);
  }

  // 把结果对象分块存入 window，返回小状态 JSON（与 baidu_serp.js/extract.js 同协议）。
  function stash(obj) {
    var s = JSON.stringify(obj);
    var CHUNK = 30000, chunks = [];
    for (var i = 0; i < s.length; i += CHUNK) chunks.push(s.substring(i, i + CHUNK));
    window.__seimiSerp = { chunks: chunks };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  }

  try {
    return extractBingSerp();
  } catch (e) {
    window.__seimiSerp = { chunks: [""] };
    return JSON.stringify({ ok: false, error: "" + e });
  }
})();
