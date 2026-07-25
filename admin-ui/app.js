// seimi-render 控制台前端逻辑
// 鉴权：若服务端启用了密码，登录拿 token 存 sessionStorage，后续请求加 Authorization: Bearer。
// 未启用密码时所有请求正常发（后端 checkAuth 直接放行）。
// i18n：文本经 window.i18n.t() 取（默认中文，右上角按钮切换）。i18n.js 在本文件之前加载。

const t = (k, v) => window.i18n.t(k, v);

const TOKEN_KEY = 'seimi_token';
const getToken = () => sessionStorage.getItem(TOKEN_KEY);
const setToken = (t) => t ? sessionStorage.setItem(TOKEN_KEY, t) : sessionStorage.removeItem(TOKEN_KEY);

// 带 token 的 fetch 包装
async function api(url, opts = {}) {
  const headers = { ...(opts.headers || {}) };
  const t = getToken();
  if (t) headers['Authorization'] = 'Bearer ' + t;
  const resp = await fetch(url, { ...opts, headers });
  if (resp.status === 401) { showLogin(); throw new Error('unauthorized'); }
  return resp;
}

// ---------- 启动入口 ----------
(async function init() {
  // 应用静态文本 i18n（默认中文，i18n.js 已据 localStorage 切到目标语言）。
  window.i18n.applyI18n();
  bindTabs();
  bindRenderForm();
  bindCookieClear();
  bindCopyButtons();
  bindResultCopy();
  bindApiTabs();
  bindLangToggle();
  bindDomainSort();   // 域名分布表头排序
  // 语言切换时重渲染动态部分（统计/cookie/MCP/API 示例；docs 若已渲染则重渲）。
  window.i18n.onLanguageChange(() => {
    if (!document.getElementById('app').classList.contains('hidden')) {
      loadStats();
      loadCookies();
      loadMcpConfig();
      renderApiExample();
      if (docsRendered && window.renderDocs) window.renderDocs();
    }
  });
  // 探测是否需要登录：请求一个受保护的 API（/status），401=需要登录，200=直接进。
  // 注意：不探测 / 本身，因为 / 始终返回管理页 HTML（壳），真正的鉴权在 API 层。
  try {
    const r = await fetch('/status');
    if (r.status === 401) {
      showLogin();
    } else {
      showApp();
    }
  } catch (_) {
    showApp(); // 连不上也先进页面，后续 API 请求会显示错误
  }
  bindLogin();
})();

// ---------- 语言切换 ----------
// 右上角按钮：点击 toggle 中/英。i18n.js 负责持久化 + 应用静态 DOM + 通知 onLanguageChange 回调。
function bindLangToggle() {
  const btn = document.getElementById('lang-toggle');
  if (!btn) return;
  btn.addEventListener('click', () => window.i18n.toggle());
}

// ---------- 登录 ----------
function showLogin() {
  document.getElementById('login-overlay').classList.remove('hidden');
  document.getElementById('app').classList.add('hidden');
}
function showApp() {
  document.getElementById('login-overlay').classList.add('hidden');
  document.getElementById('app').classList.remove('hidden');
  setConn(true);
  loadStats();
  loadCookies();
  loadMcpConfig();
  renderApiExample();   // token 状态决定示例是否带 Authorization 头
}
function bindLogin() {
  document.getElementById('login-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const pwd = document.getElementById('login-password').value;
    const errEl = document.getElementById('login-error');
    errEl.textContent = '';
    try {
      const r = await fetch('/api/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password: pwd }),
      });
      if (!r.ok) { errEl.textContent = t('login.wrongPwd'); return; }
      const data = await r.json();
      setToken(data.token);
      showApp();
    } catch (err) {
      errEl.textContent = t('login.connFail') + err.message;
    }
  });
}

// ---------- 页签切换 ----------
let docsRendered = false;
function bindTabs() {
  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
      // 接口文档页首次切到时渲染（懒加载，避免首屏开销）
      if (btn.dataset.tab === 'docs' && !docsRendered && window.renderDocs) {
        docsRendered = true;
        window.renderDocs();
      }
    });
  });
}

// ---------- 页签 1：运行时统计 ----------
let statsTimer = null;
async function loadStats() {
  if (statsTimer) clearInterval(statsTimer);
  const refresh = async () => {
    try {
      const r = await api('/status');
      if (!r.ok) return;
      const d = await r.json();
      renderStats(d);
      setConn(true);
    } catch (e) { if (e.message !== 'unauthorized') setConn(false); }
  };
  refresh();
  statsTimer = setInterval(refresh, 5000);
}
function renderStats(d) {
  const cards = [
    { label: t('stats.cards.uptime'), value: d.uptime_human || '0s' },
    { label: t('stats.cards.total'), value: d.totals?.requests ?? 0 },
    { label: t('stats.cards.success'), value: d.totals?.succeeded ?? 0, cls: 's' },
    { label: t('stats.cards.fail'), value: d.totals?.failed ?? 0, cls: 'f' },
    { label: t('stats.cards.rate'), value: ((d.totals?.success_rate ?? 0) * 100).toFixed(1) + '%' },
    { label: t('stats.cards.throughput'), value: (d.throughput_per_sec ?? 0).toFixed(2) },
  ];
  document.getElementById('stats-cards').innerHTML = cards.map(c =>
    `<div class="card"><div class="label">${c.label}</div><div class="value ${c.cls||''}">${c.value}</div></div>`
  ).join('');

  renderWorkload(d.queue);
  renderProxy(d.proxy);

  // 延迟分布
  const lat = d.latency_ms || {};
  const latItems = [
    ['min', lat.min], ['avg', Math.round(lat.avg||0)], ['p50', lat.p50],
    ['p90', lat.p90], ['p99', lat.p99], ['max', lat.max],
  ].filter(([_,v]) => v > 0);
  const latMax = Math.max(...latItems.map(([_,v]) => v), 1);
  document.getElementById('stats-latency').innerHTML = latItems.length ? latItems.map(([k,v]) =>
    `<div class="bar-row"><span class="bar-label">${k}</span>
     <div class="bar-track"><div class="bar-fill" style="width:${(v/latMax*100).toFixed(1)}%"></div></div>
     <span class="bar-val">${v} ms</span></div>`
  ).join('') : `<p class="muted small">${t('common.noSuccessTasks')}</p>`;

  // 输出类型
  const outs = d.outputs || {};
  const outItems = [['HTML', outs.html], ['Markdown', outs.markdown], ['PDF', outs.pdf], [t('stats.outputs.screenshot'), outs.screenshot]];
  const outMax = Math.max(...outItems.map(([_,v]) => v), 1);
  document.getElementById('stats-outputs').innerHTML = outItems.map(([k,v]) =>
    `<div class="bar-row"><span class="bar-label">${k}</span>
     <div class="bar-track"><div class="bar-fill" style="width:${(v/outMax*100).toFixed(1)}%"></div></div>
     <span class="bar-val">${v||0}</span></div>`
  ).join('');

  // 域名分布
  renderDomainTable(d.domains);

  // 运行环境卡片（来自 /status 的 environment 段）
  renderEnv(d.environment);
}

// ---------- 域名分布表格（排序 / 分级成功率 / 失败高亮 / 千分位）----------
// 用户可点击表头切换排序字段与方向，状态记忆在内存中（切语言/刷新数据保持）。
let domainSort = { key: 'total', dir: 'desc' };   // 默认按总请求降序，与后端一致
let lastDomainsCache = null;                       // 缓存最新数据，切语言时复用

function fmtNum(n) {                               // 千分位格式化（可读性，大数据量场景必需）
  const v = Number(n) || 0;
  return v.toLocaleString('en-US');
}

// 成功率分级：≥90% 优、70–90% 中、<70% 差、0% 全失败。
// 配色统一用「文字色 + 同色系极浅背景」组合，色块撑满单元格宽度，保证视觉一致。
// 文字色均达 WCAG AA（与浅背景对比度 ≥4.5:1）。
function rateTierClass(rate) {
  if (rate >= 90) return 'rate-excellent';
  if (rate >= 70) return 'rate-fair';
  if (rate > 0)   return 'rate-poor';
  return 'rate-zero';
}

function renderDomainTable(domains) {
  if (!domains) return;
  lastDomainsCache = domains;

  const top = domains.top || [];
  const distinct = domains.distinct || top.length;
  const shown = top.length;

  // 标题：显示 Top N（共 M 个域名），让用户知道边界与是否被截断。
  const titleEl = document.getElementById('stats-domains-count');
  if (titleEl) {
    titleEl.textContent = distinct > shown
      ? t('stats.domains.topN', { n: shown, total: distinct })
      : t('stats.domains.topNLimited', { n: shown });
  }

  // 排序：稳定排序。多列等值时按域名升序作次级排序，让顺序确定可预期。
  const sorted = top.slice().sort((a, b) => {
    const k = domainSort.key;
    let va, vb;
    if (k === 'domain') { va = a.domain || ''; vb = b.domain || ''; }
    else if (k === 'rate') {
      va = a.total ? a.succeeded / a.total : 0;
      vb = b.total ? b.succeeded / b.total : 0;
    } else { va = a[k] || 0; vb = b[k] || 0; }
    let cmp;
    if (k === 'domain') cmp = va.localeCompare(vb);
    else cmp = va - vb;
    if (domainSort.dir === 'desc') cmp = -cmp;
    if (cmp === 0) {
      // 次级排序：总请求降序 → 域名升序，保证同量级行序稳定。
      const c2 = (b.total || 0) - (a.total || 0);
      return c2 !== 0 ? c2 : (a.domain || '').localeCompare(b.domain || '');
    }
    return cmp;
  });

  const tbody = document.querySelector('#stats-domains tbody');
  if (!shown) {
    tbody.innerHTML = `<tr><td colspan="6" class="muted">${t('stats.domains.empty')}</td></tr>`;
  } else {
    tbody.innerHTML = sorted.map((dm, i) => {
      const rate = dm.total ? (dm.succeeded / dm.total * 100) : 0;
      const rateStr = rate.toFixed(1);
      const rc = rateTierClass(rate);
      const failCls = (dm.failed || 0) > 0 ? 'cell-failed' : '';
      const domain = esc(dm.domain || '');
      return `<tr>
        <td class="col-rank">${i + 1}</td>
        <td class="col-domain" title="${domain}">${domain}</td>
        <td class="num">${fmtNum(dm.total)}</td>
        <td class="num">${fmtNum(dm.succeeded)}</td>
        <td class="num ${failCls}">${fmtNum(dm.failed)}</td>
        <td class="num"><span class="rate-badge ${rc}">${rateStr}%</span></td>
      </tr>`;
    }).join('');
  }

  // 同步表头排序指示器（箭头 + aria-sort 无障碍标注）。
  document.querySelectorAll('#stats-domains th.sortable').forEach(th => {
    const k = th.dataset.sort;
    th.classList.toggle('sort-active', k === domainSort.key);
    th.classList.toggle('sort-asc',  k === domainSort.key && domainSort.dir === 'asc');
    th.classList.toggle('sort-desc', k === domainSort.key && domainSort.dir === 'desc');
    th.setAttribute('aria-sort',
      k === domainSort.key
        ? (domainSort.dir === 'asc' ? 'ascending' : 'descending')
        : 'none');
  });
}

// 表头点击排序：同一列重复点击切换升降序，不同列切过去默认降序（domain 默认升序更符合直觉）。
function bindDomainSort() {
  const table = document.getElementById('stats-domains');
  if (!table || table.dataset.sortBound) return;
  table.dataset.sortBound = '1';
  table.querySelectorAll('th.sortable').forEach(th => {
    th.addEventListener('click', () => {
      const k = th.dataset.sort;
      if (domainSort.key === k) {
        domainSort.dir = domainSort.dir === 'asc' ? 'desc' : 'asc';
      } else {
        domainSort.key = k;
        domainSort.dir = (k === 'domain') ? 'asc' : 'desc';
      }
      renderDomainTable(lastDomainsCache);
    });
  });
}

// ---------- 页签 1.6：运行环境（OS/硬件/构建信息 + 实时占用）----------
let lastEnvMarkdown = '';

function renderEnv(env) {
  const el = document.getElementById('stats-env');
  if (!el) return;
  if (!env) { el.innerHTML = `<p class="muted small">${t('common.noData')}</p>`; return; }
  lastEnvMarkdown = env.markdown || '';

  const sampled = env.sampled_at_ms > 0;
  const rows = [
    [t('stats.env.os'),       `${esc(env.os_pretty || '')} (${esc(env.arch || '')})`],
    [t('stats.env.kernel'),   esc(env.kernel || '')],
    [t('stats.env.hostname'), esc(env.hostname || '')],
    [t('stats.env.cpu'),      `${esc(env.cpu_model || '--')} · ${env.cpu_cores_physical}${t('stats.env.coresPhys')}/${env.cpu_cores_logical}${t('stats.env.coresLogical')}`],
    [t('stats.env.memory'),   `${env.memory_total_mb} MB`],
    [t('stats.env.gpu'),      env.has_gpu ? t('stats.env.gpuYes') : t('stats.env.gpuNo')],
    [t('stats.env.qt'),       esc(env.qt_version || '')],
    [t('stats.env.build'),    `${esc(env.build_time || '')} · ${esc(env.git_commit || '')}`],
    [t('stats.env.cpuUsage'), sampled ? `${(env.cpu_percent || 0).toFixed(1)}%` : '--'],
    [t('stats.env.rss'),      sampled ? `${env.memory_rss_mb} MB (${(env.memory_percent || 0).toFixed(1)}%)` : '--'],
  ];
  el.innerHTML = '<table class="env-table"><tbody>' +
    rows.map(([k, v]) => `<tr><td class="k">${k}</td><td class="v">${v}</td></tr>`).join('') +
    '</tbody></table>';

  const sampledEl = document.getElementById('stats-env-sampled');
  if (sampledEl) {
    sampledEl.textContent = sampled
      ? t('stats.env.sampledAt', { ts: new Date(env.sampled_at_ms).toLocaleTimeString() })
      : '';
  }
}

async function copyEnv() {
  if (!lastEnvMarkdown) return;
  // 优先用现代 Clipboard API
  try {
    await navigator.clipboard.writeText(lastEnvMarkdown);
    showCopyStatus(t('stats.env.copied'), 'ok');
    return;
  } catch (e) {
    // 非安全上下文（http://非localhost）下 clipboard 不可用，走兜底
  }
  // 兜底：隐藏 textarea + execCommand
  const ta = document.createElement('textarea');
  ta.value = lastEnvMarkdown;
  ta.style.position = 'fixed';
  ta.style.top = '-9999px';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e2) {}
  document.body.removeChild(ta);
  showCopyStatus(ok ? t('stats.env.copied') : t('stats.env.copyFail'), ok ? 'ok' : 'err');
}

function showCopyStatus(msg, cls) {
  const s = document.getElementById('env-copy-status');
  if (!s) return;
  s.textContent = msg;
  s.className = 'copy-status ' + cls;
  setTimeout(() => {
    s.textContent = '';
    s.className = 'copy-status';
  }, 2000);
}

// 拷贝按钮绑定（DOMContentLoaded 后执行，确保元素存在）
document.addEventListener('DOMContentLoaded', () => {
  const btn = document.getElementById('env-copy-btn');
  if (btn) btn.addEventListener('click', copyEnv);
});

// ---------- 页签 1.3：渲染负载（worker 活跃 / 队列堆积 / 峰值）----------
// 数据来自 /status 的 queue 字段。worker 总数 = 并发槽位（--concurrency，静态）；
// busy = 正在渲染的 worker（= running 任务）；pending = 当前堆积；peak_pending = 历史峰值。
function renderWorkload(q) {
  const box = document.getElementById('stats-workload');
  if (!box) return;
  if (!q) { box.innerHTML = `<p class="muted small">${t('common.noData')}</p>`; return; }
  const workers = q.workers ?? 0;
  const busy = q.workers_busy ?? q.running ?? 0;
  const pending = q.pending ?? 0;
  const peak = q.peak_pending ?? 0;

  // worker 活跃进度条：busy / workers 占比。
  const wPct = workers > 0 ? Math.min(busy / workers * 100, 100) : 0;
  const wBar = workers > 0
    ? `<div class="bar-row" style="margin-top:8px"><span class="bar-label">${t('workload.active')}</span>
        <div class="bar-track"><div class="bar-fill" style="width:${wPct.toFixed(1)}%"></div></div>
        <span class="bar-val">${busy} / ${workers}</span></div>`
    : `<p class="muted small" style="margin-top:6px">${t('workload.poolNotStarted')}</p>`;

  // pending 堆积 + 峰值标注。堆积 >0 用 warn 色提示（提交速度 > 渲染速度）。
  const pendingCls = pending > 0 ? ' style="color:var(--warn)"' : '';
  box.innerHTML =
    '<div class="proxy-line">' +
      '<span class="proxy-kv"><span class="pk">' + t('workload.workerActive') + '</span><span class="pv"' + pendingCls + '>' + busy + ' / ' + workers + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('workload.queuePending') + '</span><span class="pv"' + (pending > 0 ? ' style="color:var(--warn)"' : '') + '>' + pending + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('workload.queuePeak') + '</span><span class="pv mono">' + peak + '</span></span>' +
    '</div>' +
    wBar +
    '<p class="muted small" style="margin-top:10px">' + t('workload.desc') + '</p>';
}

// ---------- 页签 1.5：网络代理生效状态 ----------
// 数据来自 /status 的 proxy 字段（与 GET /proxy 同源，密码脱敏）。
// 这里只做展示；配置代理请用启动参数 --proxy 或 POST /proxy（接口文档页有示例）。
function renderProxy(p) {
  const badge = document.getElementById('stats-proxy-badge');
  const body = document.getElementById('stats-proxy-body');
  if (!badge || !body) return;
  if (!p || p.type === 'direct' || p.enabled === false) {
    badge.textContent = t('proxy.on');
    badge.className = 'proxy-badge proxy-badge-off';
    body.innerHTML =
      '<div class="proxy-line"><span class="proxy-kv"><span class="pk">' + t('proxy.kv.mode') + '</span><span class="pv">' + t('proxy.kv.directMode') + '</span></span></div>' +
      '<p class="muted small" style="margin-top:10px">' + t('proxy.directDesc') + '</p>';
    return;
  }
  const typeLabel = p.type === 'socks5' ? t('proxy.socks5On') : (p.type === 'http' ? t('proxy.httpOn') : t('proxy.genericOn'));
  badge.textContent = typeLabel;
  badge.className = 'proxy-badge proxy-badge-on';
  const authInfo = p.password_set
    ? '<span class="proxy-auth proxy-auth-set">' + t('proxy.authSet') + '</span>'
    : '<span class="proxy-auth proxy-auth-none">' + t('proxy.authNone') + '</span>';
  body.innerHTML =
    '<div class="proxy-line">' +
      '<span class="proxy-kv"><span class="pk">' + t('proxy.kv.protocol') + '</span><span class="pv">' + esc(p.type.toUpperCase()) + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('proxy.kv.host') + '</span><span class="pv mono">' + esc(p.host || '—') + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('proxy.kv.port') + '</span><span class="pv mono">' + esc(String(p.port || 0)) + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('proxy.kv.user') + '</span><span class="pv mono">' + (p.user ? esc(p.user) : '—') + '</span></span>' +
      '<span class="proxy-kv"><span class="pk">' + t('proxy.kv.auth') + '</span>' + authInfo + '</span>' +
    '</div>' +
    '<p class="muted small" style="margin-top:10px">' + t('proxy.desc') + '</p>';
}

// ---------- 页签 2：渲染测试台 ----------
function bindRenderForm() {
  document.getElementById('render-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const { url } = readRenderForm();
    if (!url) return;
    const body = buildRenderBody();
    const btn = document.getElementById('render-submit');
    const statusEl = document.getElementById('render-status');
    const resultEl = document.getElementById('render-result');
    btn.disabled = true; btn.textContent = t('render.submitting');
    statusEl.className = 'render-status'; statusEl.textContent = t('render.status.working');
    resultEl.innerHTML = '';
    try {
      const r = await api('/render', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const d = await r.json();
      if (d.state === 'succeeded') {
        statusEl.className = 'render-status ok';
        statusEl.textContent = t('render.status.ok', { ms: d.elapsed_ms, id: d.task_id });
        renderResult(d, body.output);
      } else {
        statusEl.className = 'render-status err';
        statusEl.textContent = t('render.status.fail', {
          state: (d.state === 'failed' ? t('render.status.failed') : t('render.status.incomplete')),
          err: (d.error || d.state), ms: d.elapsed_ms,
        });
      }
    } catch (err) {
      statusEl.className = 'render-status err';
      statusEl.textContent = t('render.status.reqFail') + err.message;
    } finally {
      btn.disabled = false; btn.textContent = t('render.btn');
    }
  });

  // 拷贝 curl 命令：按当前表单参数生成等价的 POST /render curl。
  document.getElementById('render-copy-curl').addEventListener('click', async () => {
    const { url } = readRenderForm();
    const body = buildRenderBody();
    const btn = document.getElementById('render-copy-curl');
    // URL 为空时给出占位示例，确保拷出的命令可直接跑。
    if (!url) body.url = 'https://example.com/';
    const base = window.location.origin;
    const tok = getToken();
    const authHeader = tok ? ` -H "Authorization: Bearer ${tok}"` : '';
    const curl = `curl -X POST ${base}/render \\
  -H "Content-Type: application/json"${authHeader} \\
  -d '${JSON.stringify(body)}'`;
    const ok = await copyText(curl);
    const orig = btn.textContent;
    btn.textContent = ok ? t('render.curlDone') : t('render.curlFail');
    btn.classList.add('copied');
    setTimeout(() => { btn.textContent = orig; btn.classList.remove('copied'); }, 1500);
  });
}

// 读取渲染表单参数（提交渲染与拷贝 curl 共用，保证两者生成的请求体一致）。
function readRenderForm() {
  const url = document.getElementById('render-url').value.trim();
  const settle = parseInt(document.getElementById('render-settle').value) || 2000;
  const longpoll = parseInt(document.getElementById('render-longpoll').value) || 30000;
  const outputs = [];
  if (document.getElementById('out-html').checked) outputs.push('html');
  if (document.getElementById('out-markdown').checked) outputs.push('markdown');
  if (document.getElementById('out-pdf').checked) outputs.push('pdf');
  if (document.getElementById('out-screenshot').checked) outputs.push('screenshot');
  const shotFmt = document.querySelector('input[name="shot-fmt"]:checked');
  const mdAlg = document.querySelector('input[name="md-alg"]:checked');
  const extractEl = document.querySelector('input[name="extract"]:checked');
  const extractVal = extractEl ? extractEl.value : '';
  return { url, settle, longpoll, outputs, shotFmt, mdAlg, extractVal };
}

// 由表单参数组装 /render 请求体。
function buildRenderBody() {
  const { url, settle, longpoll, outputs, shotFmt, mdAlg, extractVal } = readRenderForm();
  const body = {
    url, settle_ms: settle, long_poll_ms: longpoll, output: outputs.join(','),
    format: shotFmt ? shotFmt.value : 'auto',
    md_algorithm: mdAlg ? mdAlg.value : 'conservative',
  };
  if (extractVal) body.extract = extractVal;
  return body;
}
function renderResult(d, output) {
  const el = document.getElementById('render-result');
  const wantHtml = output.includes('html');
  const wantMd = output.includes('markdown');
  const wantPdf = output.includes('pdf');
  const wantShot = output.includes('screenshot');
  let html = '';
  // PDF 下载链接（带 token，浏览器可直接打开）
  const tok = getToken();
  if (wantPdf && d.has_pdf) {
    const pdfUrl = `/pdf/${d.task_id}` + (tok ? `?token=${encodeURIComponent(tok)}` : '');
    html += `<p style="margin-bottom:12px"><a href="${pdfUrl}" target="_blank" style="color:var(--primary)">${t('render.pdfDownload', { bytes: d.pdf_bytes })}</a></p>`;
  }
  // 截图（PNG/JPEG）—— 内联预览 + 下载链接。所见即所得，区别于 PDF 的打印输出。
  // 格式由后端智能选择或用户指定，读 image_format 动态显示。
  if (wantShot && d.has_image) {
    const imgUrl = `/image/${d.task_id}` + (tok ? `?token=${encodeURIComponent(tok)}` : '');
    const fmtUpper = (d.image_format || 'png').toUpperCase();
    const algNote = d.image_format === 'jpeg' ? t('render.shotNote.jpeg') : t('render.shotNote.lossless');
    html += `<h4 style="margin:8px 0;font-size:13px">${t('render.shotPreview', { fmt: fmtUpper, bytes: d.image_bytes, note: algNote })}</h4>`;
    html += `<img src="${imgUrl}" style="max-width:100%;border:1px solid var(--border);border-radius:6px;margin-bottom:8px" alt="screenshot">`;
    html += `<p style="margin-bottom:12px"><a href="${imgUrl}" target="_blank" style="color:var(--primary)">${t('render.shotDownload', { fmt: fmtUpper, bytes: d.image_bytes })}</a></p>`;
  }
  // SERP 结构化提取结果（extract != none 时后端返回 serp_json 对象）。
  // 优先展示：搜索结果页场景下 markdown/html 是噪声，serp_json 才是干净的结构化数据。
  if (d.serp_json) {
    const sj = d.serp_json;
    if (sj.blocked) {
      html += `<h4 style="margin:8px 0;font-size:13px;color:var(--danger)">${t('render.serp.blocked')}</h4>`;
      html += `<p class="muted">${t('render.serp.blockedDesc')}</p>`;
    } else {
      const results = sj.results || [];
      const adsPart = (sj.meta && sj.meta.ads_filtered) ? t('render.serp.adsFiltered', { n: sj.meta.ads_filtered }) : '';
      html += `<h4 style="margin:8px 0;font-size:13px">${t('render.serp.results', { n: results.length, ads: adsPart })}</h4>`;
      // 编号列表
      if (results.length) {
        html += `<div class="code-block"><pre>${esc(results.map((r, i) => `${i + 1}. ${r.title}\n   ${r.url}${r.is_redirect ? '  ' + t('render.serp.redirect') : ''}${r.source ? '\n   [' + r.source + ']' : ''}${r.snippet ? '\n   ' + r.snippet : ''}`).join('\n\n'))}</pre></div>`;
      }
      // 原始 JSON（折叠，便于精确查看）
      html += `<details style="margin-top:8px"><summary class="muted" style="cursor:pointer">${t('render.serp.rawJson')}</summary><div class="code-block" style="margin-top:4px"><button type="button" class="btn-copy" data-copy-action>${t('common.copy')}</button><pre>${esc(JSON.stringify(sj, null, 2))}</pre></div></details>`;
    }
  }
  if (wantMd && d.markdown) {
    // 用 .code-block 包裹（relative 定位），右上角放 copy 按钮，复用现有
    // .btn-copy 样式（深色背景适配）。按钮用 data-copy-action 标记，
    // 由 #render-result 上的事件委托统一处理（markdown 区块每次渲染动态生成）。
    html += `<h4 style="margin:8px 0;font-size:13px">${t('render.mdLabel')}</h4>`;
    html += `<div class="code-block"><button type="button" class="btn-copy" data-copy-action>${t('mcp.token.copy')}</button><pre>${esc(d.markdown)}</pre></div>`;
  } else if (wantHtml && d.html) {
    html += `<h4 style="margin-bottom:8px;font-size:13px">${t('render.htmlPreview')}</h4>`;
    // 用 srcdoc（内联内容，非网络加载）而非 data: URL：
    // 1) 不受 CSP frame-src 约束，后端 frame-src 可保持最严格的 'none'；
    // 2) data: URL 有长度上限/编码膨胀，srcdoc 直接内联原始字节，支持任意大小；
    // 3) attrEsc 只转义属性值里的引号和尖括号，保证能正确闭合 srcdoc="..."，
    //    iframe 内部文档结构（含 <!DOCTYPE html>、<head> 等）原样保留。
    // sandbox 不开 allow-scripts/allow-forms/allow-top-navigation，
    // 渲染结果里的脚本/表单/外链跳转一律失效，渲染内容无法触及父页凭据。
    html += `<iframe srcdoc="${attrEsc(d.html)}" sandbox="allow-same-origin"></iframe>`;
  }
  el.innerHTML = html || `<p class="muted">${t('render.noOutput')}</p>`;
}

// ---------- 页签 3：Cookie 状态 ----------
async function loadCookies() {
  try {
    const r = await api('/cookies');
    if (!r.ok) return;
    const d = await r.json();
    const doms = d.domains || [];
    document.querySelector('#cookies-table tbody').innerHTML = doms.length ? doms.map(dm =>
      `<tr><td>${esc(dm.domain)}</td><td class="num">${dm.count}</td></tr>`
    ).join('') : `<tr><td colspan="2" class="muted">${t('cookies.empty')}</td></tr>`;
  } catch (e) { /* 忽略 */ }
}
function bindCookieClear() {
  document.getElementById('cookie-clear').addEventListener('click', async () => {
    if (!confirm(t('cookies.confirmClear'))) return;
    try {
      await api('/cookies', { method: 'DELETE' });
      loadCookies();
    } catch (e) { alert(t('cookies.clearFail') + e.message); }
  });
  // 永久删除：直接删除 data/cookies.dat 文件，彻底销毁本地加密存储。
  const purgeBtn = document.getElementById('cookie-purge');
  if (purgeBtn) {
    purgeBtn.addEventListener('click', async () => {
      if (!confirm(t('cookies.confirmPurge'))) return;
      try {
        await api('/cookies?permanent=1', { method: 'DELETE' });
        loadCookies();
      } catch (e) { alert(t('cookies.purgeFail') + e.message); }
    });
  }
}

// ---------- 页签 4：MCP 配置 ----------
async function loadMcpConfig() {
  // MCP 端口从启动参数来，前端无法直接知道；用当前 host + 推测（默认 8090）。
  const loc = window.location;
  const baseHost = loc.hostname;
  const mcpPort = new URLSearchParams(loc.search).get('mcp-port') || '8090';
  const mcpUrl = `http://${baseHost}:${mcpPort}/mcp`;
  document.getElementById('mcp-url').value = mcpUrl;

  // 注意：本函数内用 tok 指 token（避免与全局 i18n 快捷函数 t() 重名）。
  const tok = getToken();

  // —— 密码启用状态：以后端 /auth-status 为准（而非本地 token 残留）——
  // 之前用 if(tok) 判断，无密码启动时若 sessionStorage 残留旧 token 会误报"已启用密码"。
  let passwordEnabled = false;
  try {
    const r = await fetch('/auth-status');
    if (r.ok) {
      const d = await r.json();
      passwordEnabled = !!d.password_enabled;
    }
  } catch (e) { /* 接口异常时退回本地 token 判断 */ passwordEnabled = !!tok; }

  // —— Token 展示卡片 ——
  const statusEl = document.getElementById('token-status-text');
  const valEl = document.getElementById('token-value');
  const descEl = document.getElementById('token-desc');
  if (passwordEnabled) {
    statusEl.textContent = t('mcp.token.protected');
    statusEl.className = 'token-status on';
    valEl.value = tok || '';
    descEl.innerHTML = t('mcp.token.descOn');
  } else {
    statusEl.textContent = t('mcp.token.unprotected');
    statusEl.className = 'token-status off';
    valEl.value = '';
    descEl.textContent = t('mcp.token.descOff');
  }

  // —— MCP 配置示例（密码模式带 headers）——
  // 启用密码后，MCP 接入层也要求同一个 Authorization: Bearer <token>。
  const authHeaders = passwordEnabled ? { "Authorization": "Bearer " + tok } : undefined;
  const claudeServer = { type: "http", url: mcpUrl };
  if (authHeaders) claudeServer.headers = authHeaders;
  const claudeCfg = { mcpServers: { "seimi-render": claudeServer } };
  const cursorServer = { url: mcpUrl };
  if (authHeaders) cursorServer.headers = authHeaders;
  const cursorCfg = { mcpServers: { "seimi-render": cursorServer } };
  document.getElementById('mcp-claude').textContent = JSON.stringify(claudeCfg, null, 2);
  document.getElementById('mcp-cursor').textContent = JSON.stringify(cursorCfg, null, 2);

  const hint = document.getElementById('mcp-auth-hint');
  if (hint) {
    hint.innerHTML = passwordEnabled
      ? t('mcp.authHintOn') + t('mcp.bindNote')
      : t('mcp.authHintOff') + t('mcp.bindNote');
  }
}
function bindCopyButtons() {
  document.querySelectorAll('[data-copy-target]').forEach(btn => {
    btn.addEventListener('click', async () => {
      const target = document.getElementById(btn.dataset.copyTarget);
      // 兼容 <input>（value）和 <pre>（textContent）
      const text = target.value !== undefined ? target.value : target.textContent;
      const ok = await copyText(text);
      const orig = btn.textContent;
      btn.textContent = ok ? t('common.copied') : t('common.copyFail');
      btn.classList.add('copied');
      setTimeout(() => { btn.textContent = orig; btn.classList.remove('copied'); }, 1500);
    });
  });
}
// 渲染结果区（markdown 等）的复制按钮：内容每次渲染动态生成，用事件委托
// 绑定到容器上，无需重绑。按钮结构与代码块 copy 一致，复用 .btn-copy 样式。
function bindResultCopy() {
  const container = document.getElementById('render-result');
  if (!container) return;
  container.addEventListener('click', async (e) => {
    const btn = e.target.closest('[data-copy-action]');
    if (!btn) return;
    // 按钮与 <pre> 同处一个 .code-block 内
    const block = btn.closest('.code-block');
    const pre = block && block.querySelector('pre');
    if (!pre) return;
    const ok = await copyText(pre.textContent);
    const orig = btn.textContent;
    btn.textContent = ok ? t('common.copied') : t('common.copyFail');
    btn.classList.add('copied');
    setTimeout(() => { btn.textContent = orig; btn.classList.remove('copied'); }, 1500);
  });
}

// ---------- 接口调用示例（带 token 的 curl）----------
let currentApiTab = 'render';
function bindApiTabs() {
  document.querySelectorAll('.api-tab').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.api-tab').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      currentApiTab = btn.dataset.api;
      renderApiExample();
    });
  });
}
function renderApiExample() {
  const loc = window.location;
  const base = loc.origin;                     // http://host:8088
  const baseHost = loc.hostname;
  const httpPort = parseInt(loc.port, 10) || 8088;
  const wsPort = httpPort + 1;                 // WS 端口默认 = HTTP+1（8089），可按 --ws-port 调整
  const tok = getToken();
  const authHeader = tok ? `-H "Authorization: Bearer ${tok}"` : null;
  const authNote = tok ? t('api.hintOn') : t('api.hintOff');
  document.getElementById('api-hint').textContent = authNote;
  // 中英注释（curl 内 # 注释行）。en 为英文，zh 回退即中文原文。
  const L = window.i18n.getCurrent() === 'en' ? 'en' : 'zh';
  const C = {
    // render
    wsOn: L === 'en'
      ? `\n\n# WebSocket (port ${wsPort}, default = HTTP+1; adjust with --ws-port) also needs the token — either:\n`
        + `#   1) query on the connect URL: ws://${baseHost}:${wsPort}/?token=${tok}\n`
        + `#   2) send this as the first message after connecting: {"action":"auth","token":"${tok}"}\n`
        + `# Then send {"action":"render","url":"https://example.com/"}`
      : `\n\n# WebSocket（端口 ${wsPort}，默认 = HTTP+1，按 --ws-port 调整）同样需 token，两种方式任一：\n`
        + `#   1) 连接 URL 带 query：ws://${baseHost}:${wsPort}/?token=${tok}\n`
        + `#   2) 连接后发首条消息：{"action":"auth","token":"${tok}"}\n`
        + `# 鉴权通过后再发 {"action":"render","url":"https://example.com/"}`,
    wsOff: L === 'en'
      ? `\n\n# WebSocket (port ${wsPort}, default = HTTP+1): no password; just\n`
        + `#   ws://${baseHost}:${wsPort}/  then send {"action":"render","url":"https://example.com/"}`
      : `\n\n# WebSocket（端口 ${wsPort}，默认 = HTTP+1）：未启用密码，直接\n`
        + `#   ws://${baseHost}:${wsPort}/  然后发 {"action":"render","url":"https://example.com/"}`,
    // cookies
    cookiesView: L === 'en' ? '# View synced cookie overview' : '# 查看已同步 cookie 概览',
    cookiesSync: L === 'en' ? '# Sync cookies (used by the browser extension, or manual)' : '# 同步 cookie（浏览器插件用，或手动）',
    // pdf
    pdfStep1: L === 'en' ? '# First POST /render to get task_id, then download the PDF' : '# 先 POST /render 拿 task_id，再下载 PDF',
    pdfStep2: L === 'en' ? '# Browser can open this link directly (token via query, no header)' : '# 浏览器可直接打开下方链接（token 走 query，无需 header）',
    pdfOrCurl: L === 'en' ? '# or with curl' : '# 或 curl',
    // screenshot
    shotLine1: L === 'en' ? '# Screenshot: output=screenshot, real pixels (WYSIWYG; differs from PDF print output)' : '# 截图：output=screenshot，真实像素（所见即所得，区别于 PDF 的打印输出）',
    shotLine2: L === 'en' ? '# format options: auto(default, smart)/png/jpg — image ratio >12% picks JPEG for smaller size' : '# format 可选：auto(默认,智能选择)/png/jpg —— 图片占比>12% 选 JPEG 体积更小',
    shotLine3: L === 'en' ? '# Short pages (<8000px) use the viewport-reset method; very long pages fall back to scroll-stitch' : '# 短页（<8000px）自动用视口重置法，超长页自动降级滚动拼接',
    shotStep1: L === 'en' ? '# 1) Submit render (long_poll waits for completion, returns task_id + has_image + image_format)' : '# 1) 提交渲染（long_poll 等完成后返回 task_id + has_image + image_format）',
    shotStep2: L === 'en' ? '# 2) Download image (browser opens directly; Content-Type matches the actual format)' : '# 2) 下载图片（浏览器可直接打开，Content-Type 按实际格式）',
    // markdown
    mdLine1: L === 'en' ? '# Markdown: output=markdown, html2md conversion' : '# Markdown：output=markdown，html2md 转换',
    mdLine2: L === 'en' ? '# md_algorithm options: conservative(default, zero false-drop) / readability(Mozilla body extraction)' : '# md_algorithm 可选：conservative(默认,零误伤)/readability(Mozilla正文定位)',
    mdLine3: L === 'en' ? '#   conservative ignores only script/style/nav/iframe — never drops body content' : '#   conservative 只忽略 script/style/nav/iframe，绝对不误伤正文',
    mdLine4: L === 'en' ? '#   readability drops nav/sidebar/copyright (great for articles); auto-falls back to conservative on non-articles' : '#   readability 剔除导航/侧栏/版权（文章页质量高），非文章页自动降级保守',
    mdLine5: L === 'en' ? '# The markdown field is embedded directly in the JSON response (no separate download route)' : '# 返回的 markdown 字段直接内嵌在 JSON 响应里（不走单独下载路由）',
  };

  let curl = '';
  if (currentApiTab === 'render') {
    const body = '{"url":"https://example.com/","settle_ms":2000,"long_poll_ms":30000,"output":"markdown"}';
    const wsTokenNote = tok ? C.wsOn : C.wsOff;
    curl = `curl -X POST ${base}/render \\
  -H "Content-Type: application/json"${authHeader ? ' \\\n  ' + authHeader : ''} \\
  -d '${body}'${wsTokenNote}`;
  } else if (currentApiTab === 'status') {
    curl = `curl ${base}/status${authHeader ? ' \\\n  ' + authHeader : ''}`;
  } else if (currentApiTab === 'cookies') {
    curl = `${C.cookiesView}\ncurl ${base}/cookies${authHeader ? ' \\\n  ' + authHeader : ''}\n\n` +
           `${C.cookiesSync}\ncurl -X POST ${base}/cookies \\
  -H "Content-Type: application/json"${authHeader ? ' \\\n  ' + authHeader : ''} \\
  -d '{"cookies":[{"name":"sid","value":"abc","domain":".example.com","hostOnly":false}]}'`;
  } else if (currentApiTab === 'pdf') {
    const tk = tok ? `?token=${encodeURIComponent(tok)}` : '';
    curl = `${C.pdfStep1}\n` +
           `${C.pdfStep2}\n` +
           `${base}/pdf/<task_id>${tk}\n\n` +
           `${C.pdfOrCurl}\ncurl ${base}/pdf/<task_id>${tok ? ` -H "Authorization: Bearer ${tok}"` : ''} -o page.pdf`;
  } else if (currentApiTab === 'screenshot') {
    const tk = tok ? `?token=${encodeURIComponent(tok)}` : '';
    curl = `${C.shotLine1}\n` +
           `${C.shotLine2}\n` +
           `${C.shotLine3}\n\n` +
           `${C.shotStep1}\n` +
           `curl -X POST ${base}/render \\
  -H "Content-Type: application/json"${authHeader ? ' \\\n  ' + authHeader : ''} \\
  -d '{"url":"https://example.com/","long_poll_ms":30000,"output":"screenshot","format":"auto"}'\n\n` +
           `${C.shotStep2}\n` +
           `${base}/image/<task_id>${tk}\n\n` +
           `${C.pdfOrCurl}\ncurl ${base}/image/<task_id>${tok ? ` -H "Authorization: Bearer ${tok}"` : ''} -o page.\${fmt}`;
  } else if (currentApiTab === 'markdown') {
    curl = `${C.mdLine1}\n` +
           `${C.mdLine2}\n` +
           `#   ${C.mdLine3}\n` +
           `#   ${C.mdLine4}\n\n` +
           `curl -X POST ${base}/render \\
  -H "Content-Type: application/json"${authHeader ? ' \\\n  ' + authHeader : ''} \\
  -d '{"url":"https://www.sohu.com/a/xxx","long_poll_ms":30000,"output":"markdown","md_algorithm":"readability"}'\n\n` +
           `${C.mdLine5}`;
  }
  document.getElementById('api-example').textContent = curl;
}

// ---------- 工具 ----------
function setConn(ok) {
  document.getElementById('conn-indicator').classList.toggle('off', !ok);
}
// 统一复制：优先 Clipboard API，失败回退 execCommand（非安全上下文 http://非localhost 下 clipboard 不可用）。
// 返回 Promise<boolean>，调用方可据结果决定提示「已复制」还是「复制失败」。
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (e) {
    // 兜底：隐藏 textarea + execCommand（copyEnv 同款，见上）
  }
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.top = '-9999px';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e2) {}
  document.body.removeChild(ta);
  return ok;
}
function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
// 仅转义 HTML 属性值：必须先转义 & 和 "（否则可能闭合 srcdoc="..." 注入属性），
// 再转义 < > 让文档内标签语法保持原样、不被属性解析器吃掉。
// 不转义单引号（属性用双引号包裹）。
function attrEsc(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}
