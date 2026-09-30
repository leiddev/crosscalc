/* ===========================================================================
   crosscalc 前端逻辑

   设计原则：前端不做任何数学运算。
     · 求值：     交给后端（webview::bind("cppEvaluate")，退化时用 POST /api/eval）
     · 进制显示： 交给后端（同一个表达式用 4 种 base 各求一次，返回 all_bases）
     · 错误提示： 后端已给出中文提示 + 出错位置/长度，前端只负责排版
   前端唯一"聪明"的地方是把按键变成表达式文本，这属于输入法行为，不是计算。
   =========================================================================== */

(() => {
  'use strict';

  // ======================================================= 后端访问 ========
  // webview 存在时优先走 bind（不经 HTTP，延迟更低、不受端口影响）；
  // 在普通浏览器里打开时退化为 fetch，方便直接用浏览器调试前端。
  const hasBind = typeof window.cppEvaluate === 'function';

  async function callEngine(req) {
    if (hasBind) {
      const text = await window.cppEvaluate(req);
      return typeof text === 'string' ? JSON.parse(text) : text;
    }
    const res = await fetch('/api/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(req),
    });
    return res.json();
  }

  async function callJson(path) {
    const res = await fetch(path);
    return res.json();
  }

  // ======================================================= 状态 ============
  const state = {
    mode: 'standard',
    base: 'dec',
    wordsize: 32,
    platform: { supports_64bit: false, max_integer_bitness: 53, max_integer: 0 },
    history: [],
    // 程序员模式里"正在输入的那个数字"。
    // BIN/OCT 没有字面量语法（引擎只认 0x 和十进制），所以要靠 BIN2DEC("...")
    // 这类转换函数表达；这个状态就是用来持续改写同一个字面量的。
    pending: null, // { kind:'hex'|'oct'|'bin', start:number, end:number, digits:string }
  };

  const $ = (sel) => document.querySelector(sel);
  const input    = $('#expr');
  const resultEl = $('#result');
  const noteEl   = $('#result-note');
  const errorBox = $('#error-box');
  const errorText= $('#error-text');
  const errorCaret = $('#error-caret');

  // ======================================================= 键盘定义 ========
  // insert 可以是字符串，也可以是 (s) => string（用于随字长变化的按键）
  const QWORD_OK = () => state.platform.supports_64bit;

  const bitFunc = (name) => () => {
    // 字长为 8/16/32/64 时分别用 BITNOT8/BITNOT16/... ，
    // 这样"字长"选择器对按位取反与循环移位是真的生效的（而不是只影响显示）。
    return `${name}${state.wordsize}(`;
  };

  const KEYPADS = {
    standard: [
      [k('C', 'clear', 'util'), k('⌫', 'backspace', 'util'), k('(', '('), k(')', ')'), k('mod', 'mod(', 'fn')],
      [k('7', '7'), k('8', '8'), k('9', '9'), k('÷', '/', 'op'), k('x²', 'sqr(', 'fn')],
      [k('4', '4'), k('5', '5'), k('6', '6'), k('×', '*', 'op'), k('√', 'sqrt(', 'fn')],
      [k('1', '1'), k('2', '2'), k('3', '3'), k('−', '-', 'op'), k('xʸ', '^', 'op')],
      [k('±', 'negate', 'util'), k('0', '0'), k('.', '.'), k('+', '+', 'op'), k('=', 'equals', 'eq')],
    ],
    scientific: [
      [k('sin', 'sin(', 'fn'), k('cos', 'cos(', 'fn'), k('tan', 'tan(', 'fn'), k('π', 'pi', 'fn'), 'SPLIT',
       k('7', '7'), k('8', '8'), k('9', '9'), k('÷', '/', 'op')],
      [k('asin', 'asin(', 'fn'), k('acos', 'acos(', 'fn'), k('atan', 'atan(', 'fn'), k('e', 'e', 'fn'), 'SPLIT',
       k('4', '4'), k('5', '5'), k('6', '6'), k('×', '*', 'op')],
      [k('sinh', 'sinh(', 'fn'), k('cosh', 'cosh(', 'fn'), k('tanh', 'tanh(', 'fn'), k('|x|', 'abs(', 'fn'), 'SPLIT',
       k('1', '1'), k('2', '2'), k('3', '3'), k('−', '-', 'op')],
      [k('ln', 'ln(', 'fn'), k('log₁₀', 'log10(', 'fn'), k('eˣ', 'exp(', 'fn'), k('mod', 'mod(', 'fn'), 'SPLIT',
       k('±', 'negate', 'util'), k('0', '0'), k('.', '.'), k('+', '+', 'op')],
      [k('x²', 'sqr(', 'fn'), k('√', 'sqrt(', 'fn'), k('xʸ', '^', 'op'), k('n!', 'fac(', 'fn'), 'SPLIT',
       k('C', 'clear', 'util'), k('⌫', 'backspace', 'util'), k('(', '('), k(')', ')')],
      [k('nCr', 'combin(', 'fn'), k('nPr', 'permut(', 'fn'), k('Γ', 'tgamma(', 'fn'), k('atan2', 'atan2(', 'fn'), 'SPLIT',
       k('=', 'equals', 'eq', 4)],
    ],
    programmer: [
      [k('A', 'digitA', 'hex'), k('B', 'digitB', 'hex'), k('C', 'digitC', 'hex'), k('D', 'digitD', 'hex'), k('E', 'digitE', 'hex'), k('F', 'digitF', 'hex')],
      [k('AND', 'bitand(', 'fn'), k('OR', 'bitor(', 'fn'), k('XOR', 'bitxor(', 'fn'),
       k('NOT', bitFunc('bitnot'), 'fn'), k('<<', '<<', 'op'), k('>>', '>>', 'op')],
      [k('ROL', bitFunc('bitlrotate'), 'fn'), k('ROR', bitFunc('bitrrotate'), 'fn'),
       k('+', '+', 'op'), k('−', '-', 'op'), k('×', '*', 'op'), k('÷', '/', 'op')],
      [k('7', '7'), k('8', '8'), k('9', '9'), k('(', '('), k(')', ')'), k('⌫', 'backspace', 'util')],
      [k('4', '4'), k('5', '5'), k('6', '6'), k('C', 'clear', 'util'), k('mod', 'mod(', 'fn'), k('~', '~', 'op')],
      [k('1', '1'), k('2', '2'), k('3', '3'), k('0', '0'), k('.', '.'), k('=', 'equals', 'eq')],
    ],
  };

  function k(label, insert, cls, span) {
    return { label, insert, cls: cls || '', span: span || 1 };
  }

  // 程序员模式里，某个数字键在当前进制下是否可用
  const DIGIT_VALUE = {
    '0': 0, '1': 1, '2': 2, '3': 3, '4': 4, '5': 5, '6': 6, '7': 7,
    '8': 8, '9': 9, 'A': 10, 'B': 11, 'C': 12, 'D': 13, 'E': 14, 'F': 15,
  };

  function digitAllowed(digit) {
    const v = DIGIT_VALUE[digit];
    if (v === undefined) return true;
    const radix = { bin: 2, oct: 8, dec: 10, hex: 16 }[state.base];
    return v < radix;
  }

  // ======================================================= 渲染键盘 ========
  function renderKeypads() {
    const host = $('#keypads');
    host.innerHTML = '';
    const rows = KEYPADS[state.mode];
    const isSci = state.mode === 'scientific';

    for (const row of rows) {
      const rowEl = document.createElement('div');
      rowEl.className = 'kp-row';

      // 科学模式：一行里分成"函数列"和"数字列"，用 SPLIT 标记分隔
      const splitAt = row.indexOf('SPLIT');
      const groups = splitAt >= 0
        ? [row.slice(0, splitAt), row.slice(splitAt + 1)]
        : [row];

      for (let gi = 0; gi < groups.length; gi++) {
        let container = rowEl;
        if (groups.length > 1) {
          container = document.createElement('div');
          container.className = 'kp-col' + (gi === 0 ? ' fn-col' : '');
          rowEl.appendChild(container);
        }
        for (const key of groups[gi]) {
          container.appendChild(buildKey(key));
        }
      }
      host.appendChild(rowEl);
    }
    if (isSci) { /* 科学模式的说明在 HTML 里 */ }
  }

  function buildKey(key) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'kp-key' + (key.cls ? ' ' + key.cls : '');
    btn.textContent = key.label;
    if (key.span > 1) btn.style.flexGrow = String(key.span);

    // 禁用当前进制下非法的数字键
    const label = key.label;
    if (label.length === 1 && DIGIT_VALUE[label] !== undefined && !digitAllowed(label)) {
      btn.disabled = true;
    }

    btn.addEventListener('click', () => onKey(key));
    return btn;
  }

  // ======================================================= 输入处理 ========
  function setCaret(pos) {
    input.focus();
    input.setSelectionRange(pos, pos);
  }

  function caretPos() {
    return input.selectionStart ?? input.value.length;
  }

  function insertText(text) {
    finalizePending();
    const pos = caretPos();
    const before = input.value.slice(0, pos);
    const after = input.value.slice(pos);
    input.value = before + text + after;
    setCaret(pos + text.length);
    scheduleEval();
  }

  /** 结束"正在输入的数字"状态（按了运算符、括号等就该收尾了） */
  function finalizePending() {
    state.pending = null;
  }

  function clearAll() {
    state.pending = null;
    input.value = '';
    setCaret(0);
    scheduleEval(true);
  }

  function backspace() {
    // 若正在输入程序员模式的数字，退格先消耗这个数字的位数
    if (state.pending && state.pending.digits.length > 0) {
      const p = state.pending;
      p.digits = p.digits.slice(0, -1);
      const text = literalText(p.kind, p.digits);
      input.value = input.value.slice(0, p.start) + text + input.value.slice(p.end);
      p.end = p.start + text.length;
      if (p.digits.length === 0) {
        input.value = input.value.slice(0, p.start) + input.value.slice(p.end);
        state.pending = null;
        setCaret(p.start);
      } else {
        setCaret(p.end);
      }
      scheduleEval();
      return;
    }

    finalizePending();
    const pos = caretPos();
    if (pos === 0) return;
    const before = input.value.slice(0, pos - 1);
    input.value = before + input.value.slice(pos);
    setCaret(before.length);
    scheduleEval();
  }

  /** ± —— 把光标左边紧邻的那个数字用括号包起来再取负：5 -> -(5) */
  function negate() {
    finalizePending();
    const pos = caretPos();
    const before = input.value.slice(0, pos);
    const m = before.match(/(\d+(?:\.\d+)?)$/);
    if (m) {
      const start = pos - m[1].length;
      const wrapped = `-(${m[1]})`;
      input.value = input.value.slice(0, start) + wrapped + input.value.slice(pos);
      setCaret(start + wrapped.length);
    } else {
      insertText('-(');
      return;
    }
    scheduleEval();
  }

  /** 程序员模式：数字键（含 A-F） */
  function literalText(kind, digits) {
    if (kind === 'hex') return '0x' + (digits || '0');
    const fn = kind === 'bin' ? 'BIN2DEC' : 'OCT2DEC';
    return `${fn}("${digits}")`;
  }

  function pushProgDigit(ch) {
    // 只有在十进制下才能直接写数字（引擎原生支持）
    if (state.base === 'dec') {
      finalizePending();
      insertText(ch);
      return;
    }
    const kind = state.base; // 'hex' | 'oct' | 'bin'
    const p = state.pending;

    if (p && p.kind === kind && caretPos() === p.end) {
      // 续写当前数字
      p.digits += ch;
      const text = literalText(kind, p.digits);
      input.value = input.value.slice(0, p.start) + text + input.value.slice(p.end);
      p.end = p.start + text.length;
      setCaret(p.end);
      scheduleEval();
      return;
    }

    finalizePending();
    const pos = caretPos();
    const start = pos;
    const digits = ch;
    const text = literalText(kind, digits);
    input.value = input.value.slice(0, pos) + text + input.value.slice(pos);
    state.pending = { kind, start, end: start + text.length, digits };
    setCaret(state.pending.end);
    scheduleEval();
  }

  function onKey(key) {
    const ins = key.insert;

    if (typeof ins === 'function') { insertText(ins(state)); return; }

    switch (ins) {
      case 'clear':     return clearAll();
      case 'backspace': return backspace();
      case 'negate':    return negate();
      case 'equals':    return evaluateNow({ commit: true });
    }

    // 程序员模式的十六进制数字键 A-F
    if (/^digit[A-F]$/.test(ins)) {
      pushProgDigit(ins.slice(5));
      return;
    }
    // 数字键 0-9
    if (/^[0-9]$/.test(ins)) {
      if (state.mode === 'programmer') { pushProgDigit(ins); return; }
      insertText(ins);
      return;
    }

    insertText(ins);
  }

  // ======================================================= 求值 ============
  let evalTimer = null;
  let evalSeq = 0;

  function scheduleEval(immediate) {
    if (evalTimer) clearTimeout(evalTimer);
    if (immediate) { evaluateNow(); return; }
    // 输入时实时预览；不做防抖太短的等待，键盘点击也不需要更长的延迟
    evalTimer = setTimeout(() => evaluateNow(), 90);
  }

  async function evaluateNow(opts) {
    const expr = input.value;
    const seq = ++evalSeq;

    if (expr.trim() === '') {
      showResult('0', '', false);
      hideError();
      updateBasePanel(null);
      return;
    }

    let data;
    try {
      data = await callEngine({
        expression: expr,
        mode: state.mode,
        base: state.base,
        wordsize: String(state.wordsize),
      });
    } catch (err) {
      showResult('无法求值', '后端连接失败', true);
      showError('与计算后端的连接失败：' + (err && err.message ? err.message : err), null, null, expr);
      return;
    }

    if (seq !== evalSeq) return; // 已经有更新的请求了，丢弃这个结果

    if (data.ok) {
      showResult(data.display, noteFor(data), false);
      hideError();
      updateBasePanel(data);
      if (opts && opts.commit) {
        pushHistory(expr, data.display, false);
      }
    } else {
      showResult('—', '', true);
      showError(data.error, data.error_pos, data.error_len, expr);
      updateBasePanel(null);
      if (opts && opts.commit) {
        pushHistory(expr, data.error.split('\n')[0], true);
      }
    }
  }

  function noteFor(data) {
    const bits = [];
    if (state.mode === 'programmer') {
      bits.push(`${state.wordsize} 位`);
      if (!data.integral) bits.push('结果不是整数，已按十进制显示');
    }
    return bits.join(' · ');
  }

  function showResult(text, note, isError) {
    resultEl.textContent = text;
    resultEl.classList.toggle('is-error', !!isError);
    noteEl.textContent = note || '';
  }

  // ---------------------------------------------------------- 错误展示 ----
  function showError(message, pos, len, expr) {
    errorText.textContent = message;
    errorCaret.textContent = '';

    if (typeof pos === 'number' && pos >= 0 && pos <= expr.length) {
      const n = Math.max(1, len || 1);
      const before = expr.slice(0, pos);
      const bad = expr.slice(pos, pos + n);
      const after = expr.slice(pos + n);

      // 用等宽字体对齐：第一行标出出错片段，第二行用 ^ 指到它下面
      const line1 = document.createElement('span');
      line1.append(before);
      const mark = document.createElement('span');
      mark.className = 'bad';
      mark.textContent = bad;
      line1.append(mark, after);

      const line2 = document.createElement('span');
      line2.textContent = ' '.repeat(pos) + '^'.repeat(Math.max(1, bad.length));

      errorCaret.append(line1, document.createTextNode('\n'), line2);
    }
    errorBox.hidden = false;
  }

  function hideError() {
    errorBox.hidden = true;
  }

  // ------------------------------------------------- 程序员模式进制面板 ----
  function updateBasePanel(data) {
    const ids = { hex: '#base-hex', dec: '#base-dec', oct: '#base-oct', bin: '#base-bin' };
    const bases = data && data.ok && data.all_bases ? data.all_bases : null;

    for (const b of Object.keys(ids)) {
      const el = $(ids[b]);
      el.textContent = bases ? (bases[b] ?? '—') : '—';
      document.querySelector(`.base-row[data-base="${b}"]`)
        .setAttribute('data-active', String(b === state.base));
    }
    for (const btn of document.querySelectorAll('.seg-btn[data-base]')) {
      btn.setAttribute('aria-pressed', String(btn.dataset.base === state.base));
    }
    for (const btn of document.querySelectorAll('.seg-btn[data-wordsize]')) {
      btn.setAttribute('aria-pressed', String(Number(btn.dataset.wordsize) === state.wordsize));
    }

    // QWORD 在当前平台上是否可用
    const qwordBtn = $('#btn-qword');
    if (qwordBtn) {
      const ok = QWORD_OK();
      qwordBtn.disabled = !ok;
      qwordBtn.title = ok
        ? '64 位整数（本平台可精确表示）'
        : '本平台的计算引擎基于 double，只能精确表示 2^53−1 以内的整数，因此不支持 QWORD';
    }

    const hint = $('#prog-hint');
    if (hint) {
      if (!QWORD_OK()) {
        hint.className = 'prog-hint warn';
        hint.textContent =
          '注意：本平台不支持 64 位（QWORD）整数运算 —— 引擎以 double 为数值类型，' +
          '只能精确表示 2^53−1 以内的整数。请使用 BYTE / WORD / DWORD。';
      } else if (state.base === 'bin' || state.base === 'oct') {
        hint.className = 'prog-hint';
        hint.textContent =
          '二进制/八进制没有字面量写法，数字键会自动生成 ' +
          (state.base === 'bin' ? 'BIN2DEC("…")' : 'OCT2DEC("…")') + ' 形式；' +
          '十六进制写作 0xFF。';
      } else {
        hint.className = 'prog-hint';
        hint.textContent = '按位与/或/异或用 BITAND/BITOR/BITXOR（注意：运算符 & | 在引擎里是逻辑运算）。';
      }
    }
  }

  // ---------------------------------------------------------- 历史记录 ----
  const HISTORY_LIMIT = 60;

  function pushHistory(expr, result, isError) {
    const last = state.history[0];
    if (last && last.expr === expr && last.result === result) return;
    state.history.unshift({ expr, result, isError });
    if (state.history.length > HISTORY_LIMIT) state.history.length = HISTORY_LIMIT;
    renderHistory();
  }

  function renderHistory() {
    const host = $('#history');
    host.innerHTML = '';
    $('#history-empty').hidden = state.history.length > 0;
    state.history.forEach((item) => {
      const li = document.createElement('li');
      const e = document.createElement('div');
      e.className = 'h-expr';
      e.textContent = item.expr;
      const r = document.createElement('div');
      r.className = 'h-res' + (item.isError ? ' h-err' : '');
      r.textContent = (item.isError ? '⚠ ' : '= ') + item.result;
      li.append(e, r);
      li.title = '点击把这条表达式填回输入框';
      li.addEventListener('click', () => {
        state.pending = null;
        input.value = item.expr;
        setCaret(item.expr.length);
        scheduleEval();
      });
      host.appendChild(li);
    });
  }

  // ======================================================= 模式切换 ========
  function setMode(mode) {
    state.mode = mode;
    state.pending = null;
    document.body.dataset.mode = mode;
    for (const tab of document.querySelectorAll('.mode-tab')) {
      tab.setAttribute('aria-selected', String(tab.dataset.mode === mode));
    }
    renderKeypads();
    updateBasePanel(null);
    evaluateNow();
  }

  // ======================================================= 启动 ============
  async function init() {
    // 先拿到平台能力，才能正确决定 QWORD 是否可用
    try {
      state.platform = await callJson('/api/platform');
    } catch (_) { /* 保持默认值 */ }

    try {
      const info = await callJson('/api/info');
      $('#app-version').textContent = 'v' + info.version;
      renderAbout(info);
    } catch (_) { /* 忽略 */ }

    for (const tab of document.querySelectorAll('.mode-tab')) {
      tab.addEventListener('click', () => setMode(tab.dataset.mode));
    }
    for (const btn of document.querySelectorAll('.seg-btn[data-base]')) {
      btn.addEventListener('click', () => {
        if (state.base === btn.dataset.base) return;
        state.base = btn.dataset.base;
        state.pending = null;
        renderKeypads();
        evaluateNow();
      });
    }
    for (const btn of document.querySelectorAll('.seg-btn[data-wordsize]')) {
      btn.addEventListener('click', () => {
        if (btn.disabled) return;
        state.wordsize = Number(btn.dataset.wordsize);
        state.pending = null;
        renderKeypads();
        evaluateNow();
      });
    }
    for (const row of document.querySelectorAll('.base-row')) {
      row.addEventListener('click', () => {
        const b = row.dataset.base;
        if (state.base === b) return;
        state.base = b;
        state.pending = null;
        renderKeypads();
        evaluateNow();
      });
    }

    $('#btn-clear-history').addEventListener('click', () => {
      state.history = [];
      renderHistory();
    });

    // ---- 弹窗 ----
    $('#btn-functions').addEventListener('click', async () => {
      $('#dlg-functions').showModal();
      try {
        const data = await callJson('/api/functions');
        $('#functions-text').textContent = data.text;
      } catch (err) {
        $('#functions-text').textContent = '加载失败：' + err;
      }
    });
    $('#btn-about').addEventListener('click', () => $('#dlg-about').showModal());

    // ---- 输入框事件 ----
    input.addEventListener('input', () => { state.pending = null; scheduleEval(); });
    input.addEventListener('keydown', onInputKeydown);

    // ---- 鼠标/键盘输入时也同步键盘高亮无关，略 ----
    renderKeypads();
    updateBasePanel(null);
    if (!hasBind) {
      showResult('0', '（浏览器直连模式：未检测到 webview 绑定）', false);
    }
  }

  function onInputKeydown(ev) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      evaluateNow({ commit: true });
      return;
    }
    if (ev.key === 'Escape') {
      ev.preventDefault();
      clearAll();
      return;
    }
    if (ev.ctrlKey && (ev.key === 'l' || ev.key === 'L')) {
      ev.preventDefault();
      clearAll();
    }
  }

  function renderAbout(info) {
    const p = info.platform || {};
    const yes = (b) => (b ? '支持' : '不支持');
    $('#about-body').innerHTML = `
      <p>版本 <code>${info.version}</code>　平台 <code>${info.os}</code></p>
      <p class="kv">
        进程 ${info.pid}　运行时长 ${Math.round((info.uptime_ms || 0) / 1000)} 秒<br>
        webview ${info.webview}　cpp-httplib ${info['cpp-httplib']}<br>
        计算引擎 tinyexpr-plusplus ${info['tinyexpr-plusplus']}
      </p>
      <h3>整数精度（影响程序员模式）</h3>
      <p>
        精确整数位数：<code>${p.max_integer_bitness}</code><br>
        最大精确整数：<code>${Number(p.max_integer).toLocaleString('en-US')}</code><br>
        64 位整数（QWORD）：<strong>${yes(p.supports_64bit)}</strong><br>
        按位运算数值上限：<code>${Number(p.max_bitops_value).toLocaleString('en-US')}</code>（2^48−1）
      </p>
      <h3>已知限制</h3>
      <ul>
        <li>三角函数按<strong>弧度</strong>计算，不提供角度制（DEG/GRAD）。</li>
        <li>不提供百分比（<code>%</code> 是取模运算）。</li>
        <li>引擎没有 <code>log2</code>、<code>sec</code>、<code>csc</code>、
            <code>asinh/acosh/atanh</code>、<code>gcd/lcm</code>，本程序也不自行实现。</li>
        <li>按位与/或/非请用 <code>BITAND</code>／<code>BITOR</code>／<code>BITXOR</code>／<code>BITNOT</code>；
            运算符 <code>&amp;</code> 与 <code>|</code> 在引擎里是<strong>逻辑</strong>运算。</li>
        <li>二进制/八进制字面量没有语法，需用 <code>BIN2DEC()</code>／<code>OCT2DEC()</code>；十六进制写作 <code>0x</code>。</li>
      </ul>
    `;
  }

  document.addEventListener('DOMContentLoaded', init);
})();
