// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

// 后台定时自动同步 service worker（MV3）。
//
// 为什么要有它：popup 只在打开期间存活，「定时自动同步」要在 popup 关闭后继续跑，
// 必须有后台上下文。MV3 service worker 是事件驱动、空闲即被杀的，周期任务不能用
// setInterval（SW 一死定时器就没了），标准做法是 chrome.alarms —— 每次到点会把
// SW 唤醒后再回调，天然适配短命 SW。
//
// 配置全部存 chrome.storage.local（popup 写、这里读，改完落库即生效）：
//   autoSyncEnabled     bool    总开关。默认不开 = 不注册 alarm，零后台活动（与旧版行为一致）
//   autoSyncIntervalSec number  周期（秒），默认 30。chrome.alarms 最小周期 30s（Chrome 120+，
//                               更老版本浏览器会静默钳到 1 分钟）
//   excludedDomains     string[] popup 里被取消勾选的域名（自动同步尊重这份排除集）
//   endpoint / token    同 popup，每次触发时现读（热改即生效）
// 每次触发结果写回 autoSyncLast = { at, ok, count, error }，popup 打开时展示。
// 失败不自动关开关（服务暂时没起来很常见），恢复后自然继续。

importScripts('sync-core.js');

const ALARM_NAME = 'cookie-auto-sync';
const DEFAULT_INTERVAL_SEC = 30;
const MIN_INTERVAL_SEC = 30;
const MAX_INTERVAL_SEC = 86400;

function clampIntervalSec(v) {
  const n = parseInt(v, 10);
  if (!Number.isFinite(n)) return DEFAULT_INTERVAL_SEC;
  return Math.min(MAX_INTERVAL_SEC, Math.max(MIN_INTERVAL_SEC, n));
}

// 按当前配置对齐 alarm：关着就清掉；开着且周期没变就不动（避免 SW 每次被无关事件
// 唤醒都重置计时）。周期变化时 create 同名 alarm 会整体替换并重新起表。
async function ensureAlarm() {
  const cfg = await chrome.storage.local.get(['autoSyncEnabled', 'autoSyncIntervalSec']);
  const existing = await chrome.alarms.get(ALARM_NAME);
  if (!cfg.autoSyncEnabled) {
    if (existing) await chrome.alarms.clear(ALARM_NAME);
    return;
  }
  const periodMin = clampIntervalSec(cfg.autoSyncIntervalSec) / 60;
  if (existing && Math.abs(existing.periodInMinutes - periodMin) < 1e-9) return;
  chrome.alarms.create(ALARM_NAME, {
    delayInMinutes: periodMin,   // 开启后先等一个周期再首次触发（要立即同步用面板的「一键同步」）
    periodInMinutes: periodMin,
  });
}

async function doAutoSync() {
  const last = { at: Date.now(), ok: false, count: 0, error: '' };
  try {
    const cfg = await chrome.storage.local.get(['endpoint', 'token', 'excludedDomains']);
    const endpoint = (cfg.endpoint || 'http://localhost:8088').replace(/\/+$/, '');
    const payload = await gatherCookiePayload(cfg.excludedDomains);
    last.count = payload.length;
    const headers = { 'Content-Type': 'application/json' };
    if (cfg.token) headers['Authorization'] = 'Bearer ' + cfg.token;
    const resp = await fetch(`${endpoint}/cookies`, {
      method: 'POST',
      headers,
      body: JSON.stringify({ cookies: payload }),
    });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    last.ok = true;
  } catch (err) {
    last.error = String((err && err.message) || err);
  }
  await chrome.storage.local.set({ autoSyncLast: last });
}

// SW 安装/浏览器启动：按持久化配置恢复 alarm（浏览器重启后自动同步自动续上）。
// 注意监听器必须 async 并 await 到底 —— MV3 里事件处理函数的 pending Promise
// 是 SW 生命周期的延长依据，同步返回会让 Chrome 在异步工作完成前就回收 SW。
chrome.runtime.onInstalled.addListener(() => ensureAlarm());
chrome.runtime.onStartup.addListener(() => ensureAlarm());
// popup 改开关/周期 → 落库 → 这里感知并重排 alarm
chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== 'local') return;
  if (changes.autoSyncEnabled || changes.autoSyncIntervalSec) return ensureAlarm();
});
chrome.alarms.onAlarm.addListener(async (alarm) => {
  if (alarm.name === ALARM_NAME) await doAutoSync();
});
// 兜底：SW 无论被什么事件唤醒，都顺手校正一次 alarm 状态
ensureAlarm();
