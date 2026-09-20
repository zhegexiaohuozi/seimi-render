// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

// seimi-render Cookie 同步插件
//
// 流程：chrome.cookies.getAll({}) -> 按 domain 聚合 -> 渲染勾选列表（默认全选，勾选状态跨会话记忆）
//       -> 用户勾选/搜索/全选 -> 一键同步 POST /cookies -> 拉取 GET /cookies 对账
//       -> 可选开启后台定时自动同步（background.js + chrome.alarms 执行，默认关闭，
//          本文件只负责开关/周期设置与「上次自动同步」状态展示）
//
// 国际化：i18n.js 提供 t()/applyI18n()/initI18n()/toggleLang()。本文件所有面向
// 用户的文案都走 t()，语言切换后重渲染动态部分。

const $ = (id) => document.getElementById(id);

// 状态
let endpoint = 'http://localhost:8088';
let authToken = '';          // 设了 --password 时填，否则空。存 chrome.storage 记忆。
let allDomains = [];      // [{domain, count, cookies:[...]}, ...] 按 count 倒序
let selected = new Set(); // 选中的 domain 集合

// 最近一次的状态消息：记录 {key, args} 以便语言切换后重译。
let lastStatus = null;    // {key, args[]} | null
// 最近一次的连接徽章状态：'checking'|'ok'|'need-token'|'off'
let lastConnState = 'checking';

// 带 token 的 fetch 包装：token 非空时加 Authorization: Bearer。
function authFetch(url, opts = {}) {
  if (authToken) {
    opts = { ...opts };
    opts.headers = { ...(opts.headers || {}), 'Authorization': 'Bearer ' + authToken };
  }
  return fetch(url, opts);
}

// ---------- 初始化 ----------
(async function init() {
  // 先初始化语言（应用静态文案），再做其余渲染
  await initI18n();

  // 读端点 + token（记忆上次）
  const saved = await chrome.storage.local.get(['endpoint', 'token']);
  if (saved.endpoint) endpoint = saved.endpoint;
  if (saved.token) authToken = saved.token;
  $('endpoint').value = endpoint;
  $('token').value = authToken;

  $('endpoint').addEventListener('change', (e) => {
    endpoint = (e.target.value || 'http://localhost:8088').replace(/\/+$/, '');
    $('endpoint').value = endpoint;
    chrome.storage.local.set({ endpoint });
    checkConnection();
  });
  $('token').addEventListener('change', (e) => {
    authToken = e.target.value.trim();
    chrome.storage.local.set({ token: authToken });
    checkConnection();
  });

  $('select-all').addEventListener('change', onSelectAll);
  $('search').addEventListener('input', renderList);
  $('sync-btn').addEventListener('click', onSync);
  $('clear-btn').addEventListener('click', onClear);
  $('lang-btn').addEventListener('click', async () => {
    await toggleLang();
    // 静态文案已由 applyI18n() 更新；这里刷新依赖运行时状态的动态文案：
    // 列表（含空状态提示）、统计、连接徽标、状态消息、同步按钮文案。
    // 注：同步进行中（loading）时 renderList→updateSummary 会覆盖按钮文案，
    //     但语言切换是低频操作且同步极快，可接受。
    renderList();            // 内部已调 updateSummary()（含同步按钮文案）
    setConnBadge(lastConnState);
    renderAutoLast();
    if (lastStatus) renderStatus(lastStatus.key, lastStatus.args);
  });

  await initAutoSync();
  await loadCookies();
  checkConnection();
})();

// ---------- 读取浏览器全部 cookie，按 domain 聚合 ----------
async function loadCookies() {
  try {
    const cookies = await chrome.cookies.getAll({});
    const map = new Map(); // domain -> cookies[]
    for (const c of cookies) {
      // Chrome 的 domain 可能带前导点（.example.com）或不带。聚合计数时
      // 统一用 domain 原值作 key（注入时按 hostOnly 区分），展示用去掉前导点的 host。
      if (!map.has(c.domain)) map.set(c.domain, []);
      map.get(c.domain).push(c);
    }
    allDomains = Array.from(map.entries())
      .map(([domain, cs]) => ({ domain, count: cs.length, cookies: cs }))
      .sort((a, b) => b.count - a.count); // 登录态重的站排前面

    // 默认全选，但恢复上次手动取消勾选的域名（后台自动同步共用这份排除集）
    const { excludedDomains } = await chrome.storage.local.get('excludedDomains');
    const excluded = new Set(excludedDomains || []);
    selected = new Set(allDomains.filter((d) => !excluded.has(d.domain)).map((d) => d.domain));
    $('select-all').checked = selected.size === allDomains.length;

    renderList();
  } catch (err) {
    $('list').innerHTML = `<div class="empty-hint">${t('loadCookieFail', escapeHtml(err.message))}</div>`;
  }
}

// 把当前勾选状态落库为排除集（未勾选的域名），供后台自动同步尊重同一份域名选择。
// 仅在用户显式改动勾选时调用——打开 popup 本身不重置后台行为。
function persistExcluded() {
  const excluded = allDomains.filter((d) => !selected.has(d.domain)).map((d) => d.domain);
  chrome.storage.local.set({ excludedDomains: excluded });
}

// ---------- 渲染域名列表（带搜索过滤）----------
function renderList() {
  const q = $('search').value.trim().toLowerCase();
  const list = $('list');
  const filtered = q
    ? allDomains.filter((d) => d.domain.toLowerCase().includes(q))
    : allDomains;

  if (filtered.length === 0) {
    list.innerHTML = `<div class="empty-hint">${q ? t('noMatchHint') : t('noCookieHint')}</div>`;
  } else {
    list.innerHTML = filtered.map((d) => {
      const checked = selected.has(d.domain) ? 'checked' : '';
      const display = d.domain.replace(/^\./, ''); // 展示去掉前导点
      return `
        <label class="row" data-domain="${escapeAttr(d.domain)}">
          <input type="checkbox" ${checked} data-domain="${escapeAttr(d.domain)}">
          <span class="domain" title="${escapeAttr(d.domain)}">${escapeHtml(display)}</span>
          <span class="count">${d.count}</span>
        </label>`;
    }).join('');
    // 绑定行内 checkbox
    list.querySelectorAll('input[type="checkbox"]').forEach((cb) => {
      cb.addEventListener('change', (e) => {
        const dom = e.target.dataset.domain;
        if (e.target.checked) selected.add(dom);
        else selected.delete(dom);
        updateSummary();
        persistExcluded();
        // 同步全选框状态（部分选中时变未勾）
        $('select-all').checked = (selected.size === allDomains.length);
      });
    });
  }
  updateSummary();
}

// ---------- 全选切换 ----------
function onSelectAll() {
  const checked = $('select-all').checked;
  if (checked) {
    selected = new Set(allDomains.map((d) => d.domain));
  } else {
    selected.clear();
  }
  persistExcluded();
  renderList();
}

// ---------- 统计 + 同步按钮文案 ----------
function updateSummary() {
  const selDomains = selected.size;
  const selCount = allDomains
    .filter((d) => selected.has(d.domain))
    .reduce((s, d) => s + d.count, 0);
  $('summary').textContent = t('summaryFmt', selDomains, allDomains.length, selCount);
  const btn = $('sync-btn');
  btn.disabled = selCount === 0;
  btn.textContent = selCount > 0 ? t('syncBtnCount', selCount) : t('syncBtn');
}

// ---------- 同步：收集选中域的 cookie -> POST /cookies -> 对账 ----------
async function onSync() {
  const btn = $('sync-btn');
  setStatus('syncing', 'info');
  btn.classList.add('loading');
  btn.disabled = true;
  btn.textContent = t('syncing');

  // 排除集 = 当前未勾选的域名；cookie 现读浏览器最新值（与后台自动同步共用
  // sync-core.js 的 gatherCookiePayload，字段映射只有一份）。popup 打开后新
  // 出现的域名也会被带上。
  const excluded = allDomains.filter((d) => !selected.has(d.domain)).map((d) => d.domain);
  let payload = [];

  try {
    payload = await gatherCookiePayload(excluded);
    const resp = await authFetch(`${endpoint}/cookies`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cookies: payload }),
    });
    if (!resp.ok) {
      const txt = await resp.text();
      throw new Error(`HTTP ${resp.status}: ${txt}`);
    }
    const data = await resp.json();
    const stored = data.stored || 0;

    // 对账：拉服务端概览确认
    let serverTotal = '?';
    try {
      const ov = await (await authFetch(`${endpoint}/cookies`)).json();
      serverTotal = ov.total;
    } catch (_) { /* 对账失败不影响主结果 */ }

    setStatus('syncSuccess', 'success', stored, serverTotal);
    setConnBadge('ok');
  } catch (err) {
    setStatus('syncFail', 'error', err.message);
    setConnBadge('off');
  } finally {
    btn.classList.remove('loading');
    btn.disabled = payload.length === 0;
    btn.textContent = payload.length > 0 ? t('syncBtnCount', payload.length) : t('syncBtn');
  }
}

// ---------- 清空服务端 cookie ----------
async function onClear() {
  setStatus('clearing', 'info');
  try {
    const resp = await authFetch(`${endpoint}/cookies`, { method: 'DELETE' });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    if ($('auto-sync').checked) setStatus('clearSuccessAuto', 'success');
    else setStatus('clearSuccess', 'success');
    setConnBadge('ok');
  } catch (err) {
    setStatus('clearFail', 'error', err.message);
    setConnBadge('off');
  }
}

// ---------- 后台定时自动同步（由 background.js 执行，这里只做开关/周期/状态展示）----------
const AUTO_SYNC_DEFAULT_SEC = 30;
// 下限 30s = chrome.alarms 的最小周期（Chrome 120+；更老版本会被浏览器钳到 1 分钟）
const AUTO_SYNC_MIN_SEC = 30;
const AUTO_SYNC_MAX_SEC = 86400;

// 最近一次后台同步结果 {at, ok, count, error} | null。缓存供语言切换后重译。
let lastAutoSyncRecord = null;

function clampAutoSyncSec(v) {
  const n = parseInt(v, 10);
  if (!Number.isFinite(n)) return AUTO_SYNC_DEFAULT_SEC;
  return Math.min(AUTO_SYNC_MAX_SEC, Math.max(AUTO_SYNC_MIN_SEC, n));
}

async function initAutoSync() {
  const saved = await chrome.storage.local.get(['autoSyncEnabled', 'autoSyncIntervalSec', 'autoSyncLast']);
  const cb = $('auto-sync');
  const input = $('auto-interval');
  cb.checked = !!saved.autoSyncEnabled;
  input.value = clampAutoSyncSec(saved.autoSyncIntervalSec);
  input.disabled = !cb.checked;
  renderAutoLast(saved.autoSyncLast || null);

  // 开关：落库即生效 —— background.js 监听 storage 变化重排 alarm
  cb.addEventListener('change', async () => {
    const on = cb.checked;
    input.disabled = !on;
    await chrome.storage.local.set({ autoSyncEnabled: on });
    if (on) setStatus('autoSyncEnabledMsg', 'success', clampAutoSyncSec(input.value));
    else setStatus('autoSyncDisabledMsg', 'info');
    renderAutoLast();
  });

  // 周期：越界/非法输入自动钳回 30–86400 秒
  input.addEventListener('change', async () => {
    const raw = parseInt(input.value, 10);
    const sec = clampAutoSyncSec(raw);
    const adjusted = Number.isFinite(raw) && raw !== sec;
    input.value = sec;
    if (adjusted) setStatus('autoSyncIntervalClamped', 'info', AUTO_SYNC_MIN_SEC, AUTO_SYNC_MAX_SEC);
    else setStatus('autoSyncIntervalSaved', 'info', sec);
    await chrome.storage.local.set({ autoSyncIntervalSec: sec });
  });

  // popup 开着时，后台每次触发同步 → 实时刷新「上次自动同步」
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area === 'local' && changes.autoSyncLast) {
      renderAutoLast(changes.autoSyncLast.newValue || null);
    }
  });
}

// 渲染「上次自动同步」状态行。record 省略时沿用缓存；无记录时按开关状态
// 显示「等待首次触发」或整行隐藏。
function renderAutoLast(record) {
  if (record !== undefined) lastAutoSyncRecord = record;
  const el = $('auto-last');
  const enabled = $('auto-sync').checked;
  if (lastAutoSyncRecord && lastAutoSyncRecord.at) {
    const time = new Date(lastAutoSyncRecord.at).toLocaleTimeString();
    if (lastAutoSyncRecord.ok) {
      el.textContent = t('autoLastOk', time, lastAutoSyncRecord.count);
      el.className = 'auto-last ok';
    } else {
      el.textContent = t('autoLastFail', time, lastAutoSyncRecord.error || '?');
      el.className = 'auto-last err';
    }
    el.title = el.textContent;   // 行内省略时 hover 看全文
  } else if (enabled) {
    el.textContent = t('autoLastPending');
    el.className = 'auto-last';
    el.title = '';
  } else {
    el.textContent = '';
    el.className = 'auto-last';
    el.title = '';
  }
}

// ---------- 连接状态检测 ----------
async function checkConnection() {
  setConnBadge('checking');
  try {
    // 第一步：/health 探活（免鉴权）
    const h = await fetch(`${endpoint}/health`);
    if (!h.ok) { setConnBadge('off'); return; }
    // 第二步：探一个受保护接口（/cookies）判断是否需要 token
    const c = await authFetch(`${endpoint}/cookies`);
    if (c.status === 401) {
      setConnBadge('need-token');
      // 未填 token 时自动聚焦 token 输入框，提示用户
      if (!authToken) {
        $('token').focus();
        $('token').placeholder = t('tokenNeedHint');
      }
    } else {
      setConnBadge('ok');
    }
  } catch (_) {
    setConnBadge('off');
  }
}

// 连接徽章：更新 class + 内部 <span> 文案。状态记到 lastConnState 供语言切换时重绘。
function setConnBadge(state) {
  lastConnState = state;
  const el = $('conn-badge');
  el.classList.remove('badge-unknown', 'badge-ok', 'badge-err', 'badge-warn');
  const keyMap = {
    checking: 'connChecking',
    ok: 'connOk',
    'need-token': 'connNeedToken',
    off: 'connOff',
  };
  const cls = {
    checking: 'badge-unknown',
    ok: 'badge-ok',
    'need-token': 'badge-warn',
    off: 'badge-err',
  };
  el.classList.add(cls[state]);
  const span = el.querySelector('span');
  span.textContent = t(keyMap[state]);
}

// 状态消息：记下 {key, args} 并渲染。语言切换后用 renderStatus() 重译。
//   setStatus(key, type, ...args) —— key 是 i18n key，args 填充 {0}/{1}…
function setStatus(key, type, ...args) {
  lastStatus = { key, args };
  renderStatus(key, args);
  const el = $('status');
  el.className = 'status ' + (type || '');
}

// 把指定 key+args 翻译并写入 #status。不改变 class。
function renderStatus(key, args) {
  $('status').textContent = t(key, ...(args || []));
}

// ---------- HTML 转义（防注入）----------
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}
function escapeAttr(s) { return escapeHtml(s); }
