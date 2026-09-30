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

  // 清零键的标签是 `AC` 而不是 `C`，两个原因，改回去之前先看 buildKey() 那段注释：
  //   1. 按键的进制可用性是按【标签】判断的，单个 `C` 会被当成十六进制数字 12，
  //      于是清零键在 HEX 以外的进制（含标准/科学模式）里全灰、点不动；
  //   2. 程序员模式第一行本来就有一个十六进制数字键 `C`，两个 C 并存本来就容易看混。
  const bitFunc = (name) => () => {
    // 字长为 8/16/32/64 时分别用 BITNOT8/BITNOT16/... ，
    // 这样"字长"选择器对按位取反与循环移位是真的生效的（而不是只影响显示）。
    return `${name}${state.wordsize}(`;
  };

  const KEYPADS = {
    standard: [
      [k('AC', 'clear', 'util'), k('⌫', 'backspace', 'util'), k('(', '('), k(')', ')'), k('mod', 'mod(', 'fn')],
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
       k('AC', 'clear', 'util'), k('⌫', 'backspace', 'util'), k('(', '('), k(')', ')')],
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
      [k('4', '4'), k('5', '5'), k('6', '6'), k('AC', 'clear', 'util'), k('mod', 'mod(', 'fn'), k('~', '~', 'op')],
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

  /** 字长的正式叫法（程序员习惯用字节数命名，比 "32 位" 更常出现在文档里）。 */
  const WORD_NAME = { 8: 'BYTE', 16: 'WORD', 32: 'DWORD', 64: 'QWORD' };

  // ------------------------------------------------ 二进制/八进制数字输入 --
  //
  // 引擎能原生解析的字面量只有十进制和 0x 十六进制，没有 0b/0o。
  // 引擎自带的 BIN2DEC() / OCT2DEC() 不能拿来当输入通道：它们最多接受 10 个
  // 字符，且凑满 10 位时按【有符号】解释（10 位二进制、30 位八进制），
  // 所以 BIN2DEC("1111111111") 会得到 -1 —— 见 docs/design-decisions.md 第 11 条。
  //
  // 因此这里把用户敲的二进制/八进制数字按位【无算术】重排成 0x 字面量：
  //   二进制 → 每 4 位一组查表得 1 个十六进制位；
  //   八进制 → 每位先展开成 3 个二进制位，再按 4 位一组查表。
  // 全程只有查表与字符串拼接，没有任何算术，也没有精度损失。
  // 进制换算的"权威实现"仍然在后端（/api/eval 的 all_bases 字段），
  // 这里做的只是输入文本的组装。
  const BIN_TO_HEX = {
    '0000': '0', '0001': '1', '0010': '2', '0011': '3',
    '0100': '4', '0101': '5', '0110': '6', '0111': '7',
    '1000': '8', '1001': '9', '1010': 'A', '1011': 'B',
    '1100': 'C', '1101': 'D', '1110': 'E', '1111': 'F',
  };
  const OCT_TO_BIN = {
    '0': '000', '1': '001', '2': '010', '3': '011',
    '4': '100', '5': '101', '6': '110', '7': '111',
  };

  /** 把纯数字串（二进制或八进制）重排成不带 0x 前缀的十六进制串。 */
  function digitsToHex(digits, kind) {
    if (!digits || digits.length === 0) return '0';
    let bits;
    if (kind === 'bin') {
      bits = digits;
    } else {
      bits = '';
      for (const c of digits) bits += OCT_TO_BIN[c] || '000';
    }
    // 从右侧对齐，左侧补 0 到 4 的倍数
    const pad = (4 - (bits.length % 4)) % 4;
    bits = '0'.repeat(pad) + bits;

    let hex = '';
    for (let i = 0; i < bits.length; i += 4) {
      hex += BIN_TO_HEX[bits.slice(i, i + 4)];
    }
    // 去掉前导 0（至少保留一位）
    return hex.replace(/^0+(?=.)/, '') || '0';
  }

  /** 当前字长下，某个进制最多能敲多少位数字（避免输入超出字长的位数）。
   *
   *  取"能被字长完整装下"的最大位数，而不是简单向上取整：
   *  1 位八进制是 3 bit，QWORD(64) 下 22 位就是 66 bit 了，会溢出；
   *  正确答案是 21 位（63 bit）。BYTE(8) 下同理只能 2 位（6 bit）。
   *  bits 可显式传入，方便测试。
   */
  function maxDigitsForBase(kind, bits = state.wordsize) {
    if (kind === 'bin') return bits;        // 1 位二进制 = 1 bit
    if (kind === 'oct') return Math.floor(bits / 3);
    return 64;                              // 十进制/十六进制交给引擎判断
  }

  // ======================================================= 渲染键盘 ========
  function renderKeypads() {
    const host = $('#keypads');
    host.innerHTML = '';
    const rows = KEYPADS[state.mode];

    for (const row of rows) {
      const rowEl = document.createElement('div');
      rowEl.className = 'kp-row';

      // 科学模式：一行分成左右两块（函数块 | 数字块），用 SPLIT 标记分隔。
      // 两块是【并排】的，不是上下堆叠——每块里的键各占一格宽度，
      // 所以整行的高度就是一格键的高度。见 style.css 的 .kp-group 注释。
      const splitAt = row.indexOf('SPLIT');
      const groups = splitAt >= 0
        ? [row.slice(0, splitAt), row.slice(splitAt + 1)]
        : [row];
      if (groups.length > 1) rowEl.className = 'kp-row sci-split';

      for (let gi = 0; gi < groups.length; gi++) {
        let container = rowEl;
        if (groups.length > 1) {
          container = document.createElement('div');
          container.className = 'kp-group';
          rowEl.appendChild(container);
        }
        for (const key of groups[gi]) {
          container.appendChild(buildKey(key));
        }
      }
      host.appendChild(rowEl);
    }
  }

  function buildKey(key) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'kp-key' + (key.cls ? ' ' + key.cls : '');
    btn.textContent = key.label;
    if (key.span > 1) btn.style.flexGrow = String(key.span);

    // 禁用当前进制下非法的数字键（比如 DEC 下的 A-F）。
    //
    // 这里按【标签】判断，所以有一条硬约束：**除了 0-9 / A-F 的数字键之外，
    // 任何键的标签都不许是单个 0-9/A-F 字符**。否则它会被当成数字一起禁用。
    // 清零键曾经就叫 `C`，于是被当成数字 12，在 DEC/OCT/BIN 下（以及 base 固定为
    // dec 的标准/科学模式）全灰——用户点不动，只能问"这个键是干嘛的"。
    // 现在清零键叫 `AC`（两个字符，天然不参与这个判断），顺带也把程序员模式里
    // "十六进制数字 C"和"清零 C"两个同名键的歧义去掉了。
    // tests/check_frontend.py 的 check_key_labels() 会盯着这条约束。
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

  /** 光标位置。
   *
   *  这里用 typeof 判断而不是 `input.selectionStart ?? ...` —— ES2020 的 `??`
   *  在较老的运行时（例如 Ubuntu 22.04 自带的 Node 12，测试脚本会加载本文件）
   *  直接是语法错误，整个脚本都跑不起来。避免 ES2020 语法让本文件在
   *  更老的 WebView 上也能解析，代价几乎为零。
   */
  function caretPos() {
    return typeof input.selectionStart === 'number'
      ? input.selectionStart
      : input.value.length;
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

  /** 程序员模式：数字键
   *
   * 引擎只认十进制与 0x 十六进制字面量，所以二进制/八进制都先重排成 0x 形式。
   * 这样做的好处是三种进制都走同一条"字面量"通道：不受 BIN2DEC/OCT2DEC
   * 那 10 位字符上限与有符号语义的限制，64 位字长下也能精确输入。
   */
  function literalText(kind, digits) {
    if (kind === 'hex') return '0x' + (digits || '0');
    if (kind === 'bin' || kind === 'oct') {
      return '0x' + digitsToHex(digits, kind);
    }
    return digits || '0';   // 十进制原样写，引擎原生支持
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
      // 位数上限 = 当前字长能表示的位数：再敲下去高位会被字长截掉，
      // 与其悄悄丢掉用户敲的键，不如停下来说明原因。
      const maxDigits = maxDigitsForBase(kind);
      if (p.digits.length >= maxDigits) {
        flashProgHint(
          `${WORD_NAME[state.wordsize]} 字长下 ${kind === 'bin' ? '二进制' : '八进制'}` +
          `最多 ${maxDigits} 位数字，再输入会被字长截断。` +
          '如需更多位数，请先切换到更大的字长。'
        );
        return;
      }
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

  /** 这条错误现在就该给用户看吗？
   *
   *  输入框每变一次就会求值一次，而 "2+"、"sin("、"((1+2)" 这类敲到一半的表达式
   *  必然求值失败。后端会给这种失败打上 incomplete 标记：实时预览时先按住不表，
   *  只在用户按下等号/回车（commit）时才把错误摆出来。
   *  真正写错的表达式（"2+*3"、"1/0"）没有这个标记，照旧立刻报错。
   */
  function errorIsPending(data, opts) {
    return !!(data && data.incomplete) && !(opts && opts.commit);
  }

  async function evaluateNow(opts) {
    const expr = input.value;
    const seq = ++evalSeq;

    if (expr.trim() === '') {
      showResult('0', '', '');
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
      showResult('无法求值', '后端连接失败', 'error');
      showError('与计算后端的连接失败：' + (err && err.message ? err.message : err), null, null, expr);
      return;
    }

    if (seq !== evalSeq) return; // 已经有更新的请求了，丢弃这个结果

    if (data.ok) {
      showResult(data.display, noteFor(data), '');
      hideError();
      updateBasePanel(data);
      if (opts && opts.commit) {
        pushHistory(expr, data.display, false);
      }
    } else if (errorIsPending(data, opts)) {
      // 还差一点：只在结果区留一句淡提示，不弹红框、不进历史
      showResult('—', '表达式还没输完，继续输入…', 'stale');
      hideError();
      updateBasePanel(null);
    } else {
      showResult('—', '', 'error');
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

  // kind：'' 正常结果 | 'error' 出错（红） | 'stale' 暂时没有有效结果（灰）
  function showResult(text, note, kind) {
    resultEl.textContent = text;
    resultEl.classList.toggle('is-error', kind === 'error');
    resultEl.classList.toggle('is-stale', kind === 'stale');
    noteEl.textContent = note || '';
  }

  // ---------------------------------------------------------- 错误展示 ----
  /** 定位行要展示的片段：以出错处为中心截一小段，两头用 … 收尾。
   *
   *  错误框的高度是写死的（--error-slot），表达式的长度却没有上限：整条铺出来
   *  会撑出横向滚动条，滚动条又要吃掉一行高度，`^` 就被挤没了。
   *  返回 { head, bad, badWidth, tail }：head + bad + tail 就是第一行文本，
   *  bad 是标红的那段，第二行的 ^ 靠 head.length 对齐、长度是 badWidth。
   *
   *  两条不变量（tests/js_frontend_test.js 会盯着，改这里记得一起看）：
   *    · badWidth >= 1 —— 出错处为空（光标停在末尾）时也得有个 ^ 指着；
   *    · head.length + badWidth <= budget，且 head + bad + tail 也 <= budget
   *      —— 超了就会在框里横滚，横滚条又吃掉一行，`^` 等于白给。
   */
  function caretWindow(expr, pos, len, max) {
    const budget = Math.max(8, max || 72);
    const n = Math.max(1, len || 1);
    // 出错片段本身也可能长得离谱（整串粘进来的数字、一长串下划线标识符）：
    // 先把它自己截到半个预算，剩下的留给上下文，两条线才都不会超出框宽。
    const raw = expr.slice(pos, pos + n);
    const badMax = Math.max(4, Math.floor(budget / 2));
    const cut = raw.length > badMax;
    const bad = (cut ? raw.slice(0, badMax) : raw) + (cut ? '…' : '');
    const marked = Math.max(1, bad.length);
    // 减去 2 是给两头的 … 留坐位：它们也是要占一格的真字符，
    // 不先扣掉的话，"左边截断"时 head 恰好比预算多出那个 … 的长度。
    const room = Math.max(0, budget - marked - 2);
    // 先按左右各一半取上下文，哪边到头了就把余量让给另一边
    let start = Math.max(0, pos - Math.floor(room / 2));
    let end = Math.min(expr.length, start + room + n);
    start = Math.max(0, end - room - n);
    const head = (start > 0 ? '…' : '') + expr.slice(start, pos);
    const tail = expr.slice(pos + n, end) + (end < expr.length ? '…' : '');
    return { head: head, bad: bad, badWidth: marked, tail: tail };
  }

  function showError(message, pos, len, expr) {
    errorText.textContent = message;
    errorCaret.textContent = '';

    if (typeof pos === 'number' && pos >= 0 && pos <= expr.length) {
      const w = caretWindow(expr, pos, len, 72);

      // 用等宽字体对齐：第一行标出出错片段，第二行用 ^ 指到它下面
      const line1 = document.createElement('span');
      line1.append(w.head);
      const mark = document.createElement('span');
      mark.className = 'bad';
      mark.textContent = w.bad;
      line1.append(mark, w.tail);

      const line2 = document.createElement('span');
      line2.textContent = ' '.repeat(w.head.length) + '^'.repeat(w.badWidth);

      errorCaret.append(line1, document.createTextNode('\n'), line2);
    }
    errorBox.classList.remove('is-empty');
  }

  // 注意：这里不是把错误框藏掉（那样错误一来一去就会推动键盘），
  // 而是标记成空 —— 位置照样占着（高度见 style.css 的 --error-slot），
  // 只把内容清空不让它显示出来。
  function hideError() {
    errorText.textContent = '';
    errorCaret.textContent = '';
    errorBox.classList.add('is-empty');
  }

  // ------------------------------------------------- 程序员模式进制面板 ----
  function updateBasePanel(data) {
    const ids = { hex: '#base-hex', dec: '#base-dec', oct: '#base-oct', bin: '#base-bin' };
    const bases = data && data.ok && data.all_bases ? data.all_bases : null;

    for (const b of Object.keys(ids)) {
      const el = $(ids[b]);
      // 用 typeof 判断代替 `bases[b] ?? '—'`，理由同 caretPos()：避免 ES2020 语法
      const v = bases ? bases[b] : undefined;
      el.textContent = typeof v === 'string' ? v : '—';
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

    // QWORD 在当前平台上是否可用。不可用时只把按钮置灰 + 挂个 title 说明原因，
    // 不在提示区常驻一条警告：那是平台的先天限制，用户改不了，天天挂着只会
    // 占着最显眼的一行喊"你少个功能"。（title 见上）
    const hint = $('#prog-hint');
    if (hint) {
      if (state.base === 'bin' || state.base === 'oct') {
        hint.className = 'prog-hint';
        const maxDigits = maxDigitsForBase(state.base);
        hint.textContent =
          (state.base === 'bin' ? '二进制' : '八进制') +
          `数字键会无损换算成 0x 十六进制字面量（引擎不认 0b/0o），` +
          `当前 ${WORD_NAME[state.wordsize]} 字长最多输入 ${maxDigits} 位。`;
      } else {
        hint.className = 'prog-hint';
        hint.textContent = '按位与/或/异或用 BITAND/BITOR/BITXOR（注意：运算符 & | 在引擎里是逻辑运算）。';
      }
    }
  }

  /** 在程序员模式提示区临时显示一条警告，几秒后自动恢复成常规提示。
   *
   *  用于"按键被拒绝"这类瞬时反馈：例如已经敲满当前字长的位数时，
   *  直接忽略按键会让人以为键盘失灵，所以要把原因写出来。
   */
  let progHintTimer = null;
  function flashProgHint(msg) {
    const hint = $('#prog-hint');
    if (!hint) return;
    hint.className = 'prog-hint warn';
    hint.textContent = msg;
    if (progHintTimer) clearTimeout(progHintTimer);
    progHintTimer = setTimeout(() => {
      progHintTimer = null;
      updateBasePanel();
    }, 4000);
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
      showResult('0', '（浏览器直连模式：未检测到 webview 绑定）', '');
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
        <li>引擎没有 <code>0b</code>／<code>0o</code> 字面量。程序员模式下按二进制/八进制
            输入时，键盘会把数字<strong>无损换算成 <code>0x</code> 字面量</strong>。</li>
        <li>引擎自带的 <code>BIN2DEC()</code>／<code>OCT2DEC()</code>／<code>HEX2DEC()</code>
            最多接受 10 位数字，且凑满 10 位时按<strong>有符号</strong>解释
            （如 <code>BIN2DEC("1111111111")</code> 是 −1），不适合输入长数值；
            请改用 <code>0x</code> 字面量。</li>
      </ul>
    `;
  }

  // 供 Node 下的单元测试使用（浏览器里没有 module，这段不会执行）。
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = { digitsToHex, literalText, maxDigitsForBase, errorIsPending, caretWindow };
  }

  document.addEventListener('DOMContentLoaded', init);
})();
