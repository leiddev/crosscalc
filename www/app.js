'use strict';

const $ = (sel) => document.querySelector(sel);
const logEl = $('#log');

let logCount = 0;

const BADGE_TEXT = {
  req: 'HTTP 请求',
  res: 'HTTP 响应',
  native: '原生调用',
  err: '错误',
};

function clockNow() {
  return new Date().toLocaleTimeString('zh-CN', {hour12: false});
}

function pretty(text) {
  try {
    return JSON.stringify(JSON.parse(text), null, 2);
  } catch {
    return text;
  }
}

function addLog(kind, title, payload) {
  const item = document.createElement('li');
  item.className = `log__item log__item--${kind}`;

  const head = document.createElement('div');
  head.className = 'log__head';

  const badge = document.createElement('span');
  badge.className = 'log__badge';
  badge.textContent = BADGE_TEXT[kind] || kind;

  const name = document.createElement('span');
  name.className = 'log__title';
  name.textContent = title;

  const time = document.createElement('time');
  time.textContent = clockNow();

  head.append(badge, name, time);

  const body = document.createElement('pre');
  body.className = 'log__body';
  body.textContent = typeof payload === 'string' ? payload : JSON.stringify(payload, null, 2);

  item.append(head, body);
  logEl.prepend(item);

  logCount += 1;
  $('#log-count').textContent = String(logCount);
}

/* ------------------------------------------------------------------ HTTP -- */

async function request(title, method, path, body) {
  addLog('req', `${method} ${path}`, body ? body.toString() : '（无请求体）');
  try {
    const res = await fetch(path, {
      method,
      headers: body ? {'Content-Type': 'application/x-www-form-urlencoded'} : undefined,
      body: body ? body.toString() : undefined,
    });
    const text = await res.text();
    addLog('res', `${res.status} ${res.statusText} — ${title}`, pretty(text));
    return text;
  } catch (err) {
    addLog('err', `${title} 请求失败`, String(err));
    throw err;
  }
}

const API_CALLS = {
  hello: () => request('GET /api/hello', 'GET', '/api/hello'),
  time: () => request('GET /api/time', 'GET', '/api/time'),
  info: () => request('GET /api/info', 'GET', '/api/info'),
  assets: () => request('GET /api/assets', 'GET', '/api/assets'),
  echo: () =>
      request('GET /api/echo', 'GET', `/api/echo?q=${encodeURIComponent($('#echo-input').value)}`),
  add: () => {
    const form = new URLSearchParams({
      a: $('#num-a').value || '0',
      b: $('#num-b').value || '0',
    });
    return request('POST /api/add', 'POST', '/api/add', form);
  },
};

/* --------------------------------------------- 原生调用（webview::bind）-- */

const NATIVE_CALLS = {
  echo: () => window.cppNativeEcho($('#native-input').value),
  hwnd: () => window.cppNativeHandle(),
  close: () => window.cppCloseWindow(),
};

function nativeBridgeReady() {
  return typeof window.cppNativeEcho === 'function';
}

async function callNative(kind) {
  if (!nativeBridgeReady()) {
    addLog('err', '桥接不可用', '当前页面不是通过 webview 打开的，没有 window.cppNative* 绑定。');
    return;
  }
  try {
    const value = await NATIVE_CALLS[kind]();
    addLog('native', `window.cpp ${kind}()`, value);
  } catch (err) {
    addLog('err', `原生调用 ${kind}() 失败`, String(err));
  }
}

/* ---------------------------------------------------------------- 启动 -- */

function setBadges(info) {
  const badges = $('#badges');
  badges.replaceChildren();

  const items = [
    ['cpp-httplib', info['cpp-httplib'], 'badge'],
    ['webview', info.webview, 'badge'],
    ['cpp-embedlib', info['cpp-embedlib'], 'badge'],
    ['nlohmann/json', info['nlohmann/json'], 'badge'],
    ['os', info.os, 'badge'],
    [`已连接 ${location.origin}`, `PID ${info.pid}`, 'badge badge--ok'],
  ];

  for (const [label, value, cls] of items) {
    const span = document.createElement('span');
    span.className = cls;
    span.textContent = label === value ? label : `${label} ${value}`;
    badges.append(span);
  }
}

async function bootstrap() {
  // 按钮接线
  for (const btn of document.querySelectorAll('[data-call]')) {
    btn.addEventListener('click', () => API_CALLS[btn.dataset.call]());
  }
  for (const btn of document.querySelectorAll('[data-native]')) {
    btn.addEventListener('click', () => callNative(btn.dataset.native));
  }
  $('#clear-log').addEventListener('click', () => {
    logEl.replaceChildren();
    logCount = 0;
    $('#log-count').textContent = '0';
  });

  // 自动回车提交
  $('#echo-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') API_CALLS.echo();
  });
  $('#native-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') callNative('echo');
  });

  // 读取版本信息作为徽章
  try {
    const info = JSON.parse(await request('GET /api/info', 'GET', '/api/info'));
    setBadges(info);
  } catch {
    $('#badges').innerHTML = '<span class="badge badge--muted">后端未响应</span>';
  }

  if (!nativeBridgeReady()) {
    addLog(
        'err', '未检测到 webview 桥接',
        '若在普通浏览器中打开此页面，② 区域的按钮不可用（它们依赖 webview::bind 注入的函数）。');
  }

  addLog(
      'native', '页面就绪',
      `来源: ${location.origin}\n桥接: ${nativeBridgeReady() ? 'webview::bind 可用' : '不可用'}`);
}

document.addEventListener('DOMContentLoaded', bootstrap);
