// seimi-render Google 搜索结果页（SERP）结构化提取脚本。
// 由 RenderWorker::extractSerp() 在 GUI 线程注入到已加载的 Google 结果页 DOM 上运行。
// 选择器基于 2026-07 真实渲染校准（与 baidu_serp.js/bing_serp.js 同构，复用 chunk 协议）。
// Google 结构：h3 在 <a> 内且 href 即真实 URL（无 /url?q= 跳转）；VwiC3b 是 div（摘要）。
// 注意：Google 反爬最强，可能返回 reCAPTCHA 拦截页（需 blocked 检测）；web_search 内置重试。
(function () {
  function extractGoogleSerp() {
    var out = { engine: "google", blocked: false, results: [], recommend: [], meta: {} };

    // 1. 反爬拦截页检测：Google 触发 reCAPTCHA 时页面含 captcha-form/recaptcha。
    var bodyHtml = (document.body && document.body.innerHTML) || "";
    var bodyText = (document.body && document.body.innerText) || "";
    if (/recaptcha|captcha-form|异常流量|unusual traffic/i.test(bodyHtml) || bodyText.length < 50) {
      out.blocked = true;
      return stash(out);
    }

    // 2. 限定主结果区（#rso 是结果列表容器）。
    var scope = document.querySelector('#rso') || document.querySelector('#search') || document.body;

    // 3. 收集所有结果块：每个 h3 代表一条结果。
    //    Google 的 h3 在 <a> 标签内，a 的 href 即真实目标 URL（无跳转代理）。
    var h3s = scope.querySelectorAll('h3');
    var seenTitles = {};  // 去重（People-also-ask 等可能重复标题）
    var adCount = 0;

    h3s.forEach(function (h3) {
      var title = (h3.innerText || h3.textContent || "").trim();
      if (!title || seenTitles[title]) return;

      // 3a. 找标题所在的 <a>（h3 通常直接在 a 内）。
      var a = h3.closest('a') || (h3.parentElement && h3.parentElement.tagName === 'A' ? h3.parentElement : null);
      if (!a) return;
      var url = a.getAttribute("href") || "";
      // Google 现在直接给真实 https URL；极少数情况是 /url?q= 跳转（旧版），标注。
      var isRedirect = /^\/url\?/.test(url);
      // 相对 URL 补全（/url?q= 场景）。
      if (url.charAt(0) === '/') url = 'https://www.google.com' + url;
      if (!/^https?:\/\//.test(url)) return;  // 非链接（锚点等），跳过

      // 3b. 广告判定：Google 广告块含 "Sponsored" 文本或 data-plaid 属性。
      //     广告结果也在 h3 里，需从外层容器判定。
      var container = h3.closest('[data-ved]') || h3.closest('div');
      var containerHtml = container ? container.innerHTML.substring(0, 2000) : "";
      var isAd = /sponsored/i.test(containerHtml) ||
                 !!container.querySelector('[data-plaid], [role="sponsored"]');
      if (isAd) { adCount++; return; }

      seenTitles[title] = true;

      // 3c. 摘要：div.VwiC3b（实测是 div 不是 span），文本在嵌套 span 内。
      var desc = "";
      var block = h3.closest('div[data-ved]') || h3.parentElement;
      if (block) {
        var descEl = block.querySelector('.VwiC3b, [class*="VwiC3b"], .aCOpRe, span[class*="st"]');
        if (descEl) desc = (descEl.innerText || "").trim();
      }

      // 3d. 来源：cite 元素（显示 URL/域名，含面包屑 ›）。
      var source = "";
      if (block) {
        var srcEl = block.querySelector('cite, span[class*="qzEoUe"]');
        if (srcEl) source = (srcEl.innerText || "").trim();
      }

      out.results.push({
        title: title,
        url: url,
        is_redirect: isRedirect,
        snippet: desc,
        source: source,
        type: "organic"
      });
    });

    out.meta = { count: out.results.length, ads_filtered: adCount };
    return stash(out);
  }

  // 把结果对象分块存入 window，返回小状态 JSON（与 baidu/bing 同协议）。
  function stash(obj) {
    var s = JSON.stringify(obj);
    var CHUNK = 30000, chunks = [];
    for (var i = 0; i < s.length; i += CHUNK) chunks.push(s.substring(i, i + CHUNK));
    window.__seimiSerp = { chunks: chunks };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  }

  try {
    return extractGoogleSerp();
  } catch (e) {
    window.__seimiSerp = { chunks: [""] };
    return JSON.stringify({ ok: false, error: "" + e });
  }
})();
