'use strict';
/*
 * www/app.js 里纯函数的单元测试（Node 运行）。
 *
 * 只测那些不碰 DOM 的纯函数，重点是进制重排 digitsToHex()：
 * 程序员模式把用户敲的二进制/八进制数字换算成 0x 字面量，这一步一旦算错，
 * 用户看到的数值就是错的，而且界面上不容易发现。
 *
 * 判定标准不用手算的期望值，而是用 BigInt 做【独立的第二实现】：
 *   BigInt("0b1010").toString(16) 是 V8 自己实现的进制转换，
 * 我们要求 digitsToHex() 与它逐字符一致。随机取样上百组，
 * 手写期望值容易抄错，这样做既省事又严格。
 *
 * 用法：node tests/js_frontend_test.js
 */

const path = require('path');

// app.js 是给浏览器写的：加载时它会读 window.cppEvaluate（判断是否有 webview 的
// bind）、缓存几个 document.querySelector 的结果，并在最后注册 DOMContentLoaded。
// Node 里没有这些全局对象，所以先补一组最小替身让模块能被 require 进来。
// 查询一律返回 null/[]，这和"页面还没渲染"的状态等价；被测的都是纯函数，
// 不会碰到这些 DOM 引用。
globalThis.window = {};
globalThis.document = {
  addEventListener() {},
  querySelector() { return null; },
  querySelectorAll() { return []; },
  createElement() { return { style: {}, classList: { add() {}, remove() {} }, append() {}, addEventListener() {} }; },
  createTextNode() { return {}; },
  body: { dataset: {} },
};

const app = require(path.join(__dirname, '..', 'www', 'app.js'));

const { digitsToHex, maxDigitsForBase } = app;

let checks = 0;
const failures = [];

function fail(msg) {
  failures.push(msg);
}

function eq(actual, expected, label) {
  checks++;
  if (actual !== expected) {
    fail(`${label}: 期望 ${expected}，实际 ${actual}`);
  }
}

function ok(cond, label) {
  checks++;
  if (!cond) fail(label);
}

// ---------------------------------------------------------------- 固定用例 --
eq(digitsToHex('0', 'bin'), '0', 'bin 0');
eq(digitsToHex('1', 'bin'), '1', 'bin 1');
eq(digitsToHex('101', 'bin'), '5', 'bin 101');
eq(digitsToHex('1101', 'bin'), 'D', 'bin 1101');
eq(digitsToHex('1111', 'bin'), 'F', 'bin 1111');
eq(digitsToHex('10000', 'bin'), '10', 'bin 10000');
eq(digitsToHex('11111111', 'bin'), 'FF', 'bin 8 个 1');
eq(digitsToHex('00001111', 'bin'), 'F', 'bin 前导 0');
eq(digitsToHex('', 'bin'), '0', 'bin 空串');
eq(digitsToHex('0'.repeat(64), 'bin'), '0', 'bin 64 个 0');

eq(digitsToHex('7', 'oct'), '7', 'oct 7');
eq(digitsToHex('10', 'oct'), '8', 'oct 10');
eq(digitsToHex('377', 'oct'), 'FF', 'oct 377');
eq(digitsToHex('777', 'oct'), '1FF', 'oct 777（4 位八进制跨 12 位）');
eq(digitsToHex('000777', 'oct'), '1FF', 'oct 前导 0');

// 字长上限：QWORD 下 64 个 1 / 21 位八进制全 7 都应正好装满 64 位。
// 注意八进制是 21 位而不是 22 位：22 位 = 66 bit 会溢出字长
// （3*21=63 ≤ 64 < 66=3*22），这正是 maxDigitsForBase 采用向下取整的原因。
eq(digitsToHex('1'.repeat(64), 'bin'), 'F'.repeat(16), 'bin 64 个 1 = QWORD 全 1');
eq(digitsToHex('1'.repeat(64), 'bin'), 'FFFFFFFFFFFFFFFF', 'bin 64 个 1 定值');
eq(digitsToHex('7'.repeat(21), 'oct'), '7FFFFFFFFFFFFFFF', 'oct 21 位全 7 = 63 bit 全 1');

// ------------------------------------------------------- BigInt 随机对照 --
function randDigits(alphabet, len, rnd) {
  let s = '';
  for (let i = 0; i < len; i++) {
    s += alphabet[rnd() % alphabet.length];
  }
  return s;
}

// 确定性伪随机（线性同余），避免每次跑的数字不一样导致偶发失败
let seed = 20260930;
function rnd() {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed;
}

let bigintChecks = 0;
for (let i = 0; i < 200; i++) {
  const len = 1 + (i % 64);
  const s = randDigits('01', len, rnd);
  const want = BigInt('0b' + s).toString(16).toUpperCase();
  eq(digitsToHex(s, 'bin'), want, `bin 随机 ${s}`);
  bigintChecks++;
}
for (let i = 0; i < 100; i++) {
  const len = 1 + (i % 22);   // 最多 22 位八进制 = 64 位
  const s = randDigits('01234567', len, rnd);
  const want = BigInt('0o' + s).toString(16).toUpperCase();
  eq(digitsToHex(s, 'oct'), want, `oct 随机 ${s}`);
  bigintChecks++;
}

// ------------------------------------------------------------ 位数上限 --
eq(maxDigitsForBase('bin', 8), 8, 'BYTE 二进制位数');
eq(maxDigitsForBase('bin', 16), 16, 'WORD 二进制位数');
eq(maxDigitsForBase('bin', 32), 32, 'DWORD 二进制位数');
eq(maxDigitsForBase('bin', 64), 64, 'QWORD 二进制位数');
eq(maxDigitsForBase('oct', 8), 2, 'BYTE 八进制位数（3*2=6 bit，3 位会到 9 bit 溢出）');
eq(maxDigitsForBase('oct', 16), 5, 'WORD 八进制位数（3*5=15 bit）');
eq(maxDigitsForBase('oct', 32), 10, 'DWORD 八进制位数（3*10=30 bit）');
eq(maxDigitsForBase('oct', 64), 21, 'QWORD 八进制位数（3*21=63 bit）');

// 上限必须真的够用：正好取满位数时不能超出字长能表示的范围
ok(digitsToHex('1'.repeat(maxDigitsForBase('bin', 8)), 'bin').length <= 2,
   'BYTE 二进制上限对应的十六进制不超过 2 位');
ok(digitsToHex('1'.repeat(maxDigitsForBase('bin', 32)), 'bin').length <= 8,
   'DWORD 二进制上限对应的十六进制不超过 8 位');
ok(digitsToHex('7'.repeat(maxDigitsForBase('oct', 64)), 'oct').length <= 16,
   'QWORD 八进制上限对应的十六进制不超过 16 位');
ok(digitsToHex('7'.repeat(maxDigitsForBase('oct', 8)), 'oct').length <= 2,
   'BYTE 八进制上限对应的十六进制不超过 2 位');

// ---------------------------------------------------- 错误提示的时机 --
// 输入框每变一次就会求值一次，"2+"、"sin(" 这类敲到一半的表达式必然失败。
// 后端给这种失败打上 incomplete 标记，前端实时预览时必须把它压住，
// 只有按下等号/回车（commit）才允许弹红框 —— 否则打字过程会一直闪错误。
const { errorIsPending } = app;

ok(errorIsPending({ ok: false, incomplete: true }, undefined),
   '实时预览应压住"还没输完"的错误');
ok(errorIsPending({ ok: false, incomplete: true }, {}),
   '未指定 commit 时同样压住');
ok(errorIsPending({ ok: false, incomplete: true }, { commit: false }),
   '显式 commit:false 时压住');
ok(!errorIsPending({ ok: false, incomplete: true }, { commit: true }),
   '按下等号/回车时必须照实报错');
ok(!errorIsPending({ ok: false, incomplete: false }, undefined),
   '真正的输入错误（后端没打标记）立刻报');
ok(!errorIsPending({ ok: false }, undefined),
   '响应里没有该字段时按"真错误"处理');
ok(!errorIsPending({ ok: true, incomplete: false }, undefined),
   '成功的结果不压');
ok(!errorIsPending(null, undefined), '空响应不能抛异常');
ok(!errorIsPending(undefined, { commit: true }), '空响应 + commit 不能抛异常');

// ------------------------------------------------------ 错误定位行的取景 --
// 错误框高度是写死的（CSS --error-slot），长表达式整条铺出来会撑出滚动条，
// 滚动条又吃掉一行高度，`^` 箭头就被挤没了。caretWindow() 把要展示的片段
// 截到固定预算内（两头用 … 表示省略）。
// 真正要守住的不变量有两条：
//   1. 箭头指着的那一格，正好是标红片段的第一格（不然用户按箭头找不到错）；
//   2. 箭头行不超预算（超了就会横滚，等于把箭头弄丢）。
const { caretWindow } = app;

const wShort = caretWindow('1+2)', 3, 1, 72);
eq(wShort.bad, ')', '短表达式：标红片段就是出错处');
eq(wShort.head, '1+2', '短表达式：从头到尾都展示，左边不加省略号');
eq(wShort.tail, '', '短表达式：出错处在末尾，右边没有内容');
eq(wShort.badWidth, 1, '短表达式：^ 只需一个');

const longExpr = '1+2+3+4+5+6+7+8+9+10+11+12+13+14+15+16+17+18+19+20+21+22+23+24+25+26+27+28+29+30+31*(';
const tailPos = longExpr.indexOf('*(');
const wTail = caretWindow(longExpr, tailPos + 1, 1, 72);
ok(wTail.head.startsWith('…'), '长表达式：左边截断要有省略号');
const width = (w) => w.head.length + w.bad.length + w.tail.length;
ok(wTail.head.length + wTail.badWidth <= 72, '长表达式：箭头行不超预算');
ok(width(wTail) <= 72, `长表达式：整行不超过预算（实际 ${width(wTail)}）`);

// 出错处在正中间：左右都要给上下文，不能只顾一边
const wMid = caretWindow(longExpr, Math.floor(longExpr.length / 2), 1, 72);
ok(wMid.head.length > 1 && wMid.tail.length > 1, '中间的错：左右都能看到上下文');

// 出错处在开头：左边无内容，就不该白白占着省略号的位置
eq(caretWindow('nosuchfn(1)', 0, 8, 72).head, '', '开头的错：左边没有省略号');
eq(caretWindow('nosuchfn(1)', 0, 8, 72).bad, 'nosuchfn', '开头的错：标红整段');

// 出错片段自己就长得离谱（整串粘进来的数字）：也不能把 ^ 行顶出去
const wHuge = caretWindow('1+' + '9'.repeat(200), 2, 200, 72);
ok(wHuge.head.length + wHuge.badWidth <= 72, '超长出错片段：箭头行照样不超预算');
ok(wHuge.bad.endsWith('…'), '超长出错片段：标红片段内部也省略');

// 箭头行的对齐：把两行拼出来，箭头必须落在标红的第一格上
function caretAlign(w) {
  const line1 = w.head + w.bad + w.tail;
  const line2 = ' '.repeat(w.head.length) + '^'.repeat(Math.max(1, w.badWidth));
  return line2.indexOf('^') === w.head.length;
}
ok(caretAlign(wTail), '箭头与标红片段起始位置对齐（末尾出错）');
ok(caretAlign(wMid), '箭头与标红片段起始位置对齐（中间出错）');
ok(caretAlign(wHuge), '箭头与标红片段起始位置对齐（超长片段）');
ok(caretAlign(wShort), '箭头与标红片段起始位置对齐（未截断）');

// 预算再小也不能把箭头行撑爆（比如以后有人把 max 调小）
for (const budget of [8, 16, 40, 72]) {
  const w = caretWindow(longExpr, tailPos + 1, 1, budget);
  ok(w.head.length + w.badWidth <= budget, `预算 ${budget}：箭头行不超预算`);
  ok(width(w) <= budget, `预算 ${budget}：整行不超过预算（实际 ${width(w)}）`);
}

// 空表达式 / 光标停在末尾 / 越界位置都不能抛异常
eq(caretWindow('', 0, 0, 72).badWidth, 1, '空表达式：箭头行至少一个 ^');
ok(caretWindow('', 0, 0, 72).head === '', '空表达式：左边没东西可指');
eq(caretWindow('1+', 2, 1, 72).bad, '', '光标停在末尾：没有要标红的字符');
eq(caretWindow('1+', 2, 1, 72).badWidth, 1, '光标停在末尾：照样给一个 ^');
// showError() 会先挡掉越界位置，这里只是保证真被绕过去时不会炸
ok(typeof caretWindow('1+', 5, 3, 72).head === 'string', '位置越界：不抛异常');

// ------------------------------------------------------------------ 汇总 --
console.log(`JS 前端纯函数：${checks} 条断言（其中 ${bigintChecks} 条与 BigInt 对照）`);
if (failures.length > 0) {
  console.log(`失败 ${failures.length} 条：`);
  for (const f of failures.slice(0, 20)) console.log('  ✗ ' + f);
  process.exit(1);
}
console.log('ALL JS TESTS PASSED');
