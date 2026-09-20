// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

// cookie 同步共享核心：收集浏览器 cookie 并组装 POST /cookies 的 payload。
//
// 被两个上下文共用，防止两处字段映射漂移：
//   - popup.html      <script src="sync-core.js">（页面上下文，手动「一键同步」）
//   - background.js   importScripts('sync-core.js')（service worker，定时自动同步）
//
// excludedDomains：被取消勾选的 domain 原值（含前导点的原始 key）数组。手动同步与
// 后台自动同步共用同一份排除集 —— 用户刻意排除的敏感站不会被后台灌进服务端。

async function gatherCookiePayload(excludedDomains) {
  const excluded = new Set(excludedDomains || []);
  // 每次现读浏览器最新 cookie（而非 popup 打开时的快照），与后台自动同步行为一致。
  const cookies = await chrome.cookies.getAll({});
  const payload = [];
  for (const c of cookies) {
    if (excluded.has(c.domain)) continue;
    payload.push({
      name: c.name,
      value: c.value,
      domain: c.domain,
      path: c.path || '/',
      hostOnly: !!c.hostOnly,
      secure: !!c.secure,
      httpOnly: !!c.httpOnly,
      // Chrome 的 expirationDate 是自纪元起的秒数（浮点）；session cookie 为 -1。
      expirationDate: c.expirationDate > 0 ? c.expirationDate : 0,
    });
  }
  return payload;
}
