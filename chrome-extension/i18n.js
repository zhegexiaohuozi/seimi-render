// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

// seimi-render Cookie 同步插件 —— 国际化（i18n）
//
// 支持中文（zh）和英文（en）。默认跟随 Chrome 界面语言：中文（任意 zh-*）→ zh，其余 → en。
// 右上角提供一键切换按钮，选择记忆到 chrome.storage，下次打开沿用。
//
// 用法：
//   t('key')          -> 取当前语言文案
//   t('key', a, b)    -> {0}/{1} 占位替换
//   applyI18n()       -> 扫描 [data-i18n]/[data-i18n-html]/[data-i18n-ph]/[data-i18n-title] 并填充
//   setLang('en'|'zh')-> 切换语言、持久化、重新 applyI18n
//   toggleLang()      -> 在 zh/en 间切换
//   initI18n()        -> 读取存储/检测 Chrome 语言并初始化（返回 Promise<lang>）

const MESSAGES = {
  zh: {
    title: 'seimi-render Cookie 同步',
    headerTitle: 'Cookie 同步',
    connBadgeTitle: 'seimi-render 连接状态',
    connChecking: '检测中',
    connOk: '已连接',
    connNeedToken: '需 Token',
    connOff: '未连接',
    endpointLabel: 'seimi-render 端点',
    endpointPh: 'http://localhost:8088',
    tokenLabel: '访问 token <span class="hint">（设了 --password 才需填，否则留空）</span>',
    tokenPlaceholder: '（未设密码则留空）',
    tokenNeedHint: '服务端需要密码，请填 token（见管理页登录后）',
    selectAll: '全选',
    searchPlaceholder: '搜索域名…',
    summaryLoading: '载入中…',
    listLoadingHint: '正在读取浏览器 cookie…',
    noMatchHint: '没有匹配的域名',
    noCookieHint: '浏览器没有 cookie',
    loadCookieFail: '读取 cookie 失败：{0}',
    summaryFmt: '{0}/{1} 站点 · {2} cookies',
    syncBtn: '一键同步',
    syncBtnCount: '一键同步（{0}）',
    syncing: '同步中…',
    clearBtn: '清空服务端',
    clearing: '清空中…',
    tooltip: '清除 <b>seimi-render 渲染服务</b>上已同步的登录态 cookie。<br><b>不会</b>影响你 Chrome 浏览器本身的 cookie。',
    syncSuccess: '已同步 {0} cookies（服务端共 {1}）',
    syncFail: '同步失败：{0}（检查 seimi-render 是否启动、端点是否正确）',
    clearSuccess: '已清空 seimi-render 上的 cookie',
    clearFail: '清空失败：{0}',
    // 中文界面下按钮显示「English」，点击切到英文
    langToggle: 'English',
  },
  en: {
    title: 'seimi-render Cookie Sync',
    headerTitle: 'Cookie Sync',
    connBadgeTitle: 'seimi-render connection status',
    connChecking: 'Checking',
    connOk: 'Connected',
    connNeedToken: 'Need Token',
    connOff: 'Offline',
    endpointLabel: 'seimi-render endpoint',
    endpointPh: 'http://localhost:8088',
    tokenLabel: 'Access token <span class="hint">(only needed if --password is set; otherwise leave empty)</span>',
    tokenPlaceholder: '(leave empty if no password)',
    tokenNeedHint: 'Server requires a password — enter token (from admin login)',
    selectAll: 'All',
    searchPlaceholder: 'Search domains…',
    summaryLoading: 'Loading…',
    listLoadingHint: 'Reading browser cookies…',
    noMatchHint: 'No matching domains',
    noCookieHint: 'No cookies in browser',
    loadCookieFail: 'Failed to read cookies: {0}',
    summaryFmt: '{0}/{1} sites · {2} cookies',
    syncBtn: 'Sync',
    syncBtnCount: 'Sync ({0})',
    syncing: 'Syncing…',
    clearBtn: 'Clear server',
    clearing: 'Clearing…',
    tooltip: 'Clears the synced login cookies on the <b>seimi-render</b> rendering service.<br>It does <b>NOT</b> affect your Chrome browser cookies.',
    syncSuccess: 'Synced {0} cookies (server total {1})',
    syncFail: 'Sync failed: {0} (check that seimi-render is running and endpoint is correct)',
    clearSuccess: 'Cleared cookies on seimi-render',
    clearFail: 'Clear failed: {0}',
    // English 界面下按钮显示「中文」，点击切到中文
    langToggle: '中文',
  },
};

let currentLang = 'en';

// 按 Chrome 界面语言探测默认语言：中文（任意 zh-*）→ zh，其余 → en。
function detectLang() {
  const ui = ((chrome.i18n && chrome.i18n.getUILanguage) ? chrome.i18n.getUILanguage() : 'en').toLowerCase();
  return ui.startsWith('zh') ? 'zh' : 'en';
}

// 取当前语言文案；找不到时回退英文，再找不到原样返回 key。支持 {0}/{1}... 占位替换。
function t(key, ...args) {
  const table = MESSAGES[currentLang] || MESSAGES.en;
  let s = table[key];
  if (s === undefined) s = (MESSAGES.en[key] !== undefined ? MESSAGES.en[key] : key);
  if (args.length) {
    args.forEach((a, i) => { s = s.replace('{' + i + '}', a); });
  }
  return s;
}

// 扫描 DOM 上的 data-i18n* 属性并填充对应文案。
function applyI18n() {
  document.documentElement.lang = currentLang === 'zh' ? 'zh-CN' : 'en';
  document.querySelectorAll('[data-i18n]').forEach((el) => {
    el.textContent = t(el.dataset.i18n);
  });
  document.querySelectorAll('[data-i18n-html]').forEach((el) => {
    el.innerHTML = t(el.dataset.i18nHtml);
  });
  document.querySelectorAll('[data-i18n-ph]').forEach((el) => {
    el.placeholder = t(el.dataset.i18nPh);
  });
  document.querySelectorAll('[data-i18n-title]').forEach((el) => {
    el.title = t(el.dataset.i18nTitle);
  });
}

// 切换语言：持久化到 chrome.storage 并重新 applyI18n。
async function setLang(lang) {
  if (!MESSAGES[lang]) lang = 'en';
  currentLang = lang;
  await chrome.storage.local.set({ lang });
  applyI18n();
}

// 在 zh / en 之间一键切换。
async function toggleLang() {
  await setLang(currentLang === 'zh' ? 'en' : 'zh');
}

// 初始化：优先用存储里记忆的选择，否则按 Chrome 界面语言探测。
async function initI18n() {
  const saved = await chrome.storage.local.get('lang');
  if (saved.lang && MESSAGES[saved.lang]) {
    currentLang = saved.lang;
  } else {
    currentLang = detectLang();
  }
  applyI18n();
  return currentLang;
}
