/* xdebugd Web 工作台 — 零依赖原生 JS
 * 数据源: GET /api/state (增量环形缓冲, 带 *_since 游标)
 *         GET /api/disasm | /api/programs | /api/ports
 *         POST /api/load | run | restart | pause | step | over | out | cont
 *              /api/bp | /api/watch | /api/input | /api/board/{open,close,reset,send}
 */
'use strict';

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fmt = (v) => (typeof v === 'number' && !Number.isSafeInteger(v)) ? String(v) : String(v);

async function jget(u) { const r = await fetch(u); return await r.json(); }
async function jpost(u, body) {
  const r = await fetch(u, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
  return await r.json();
}

/* ---------------- 全局状态 ---------------- */
const C = { out: 0, trace: 0, wlog: 0, board: 0 };   // 环形缓冲游标
let cur = null;          // 最近一次 /api/state
let prevRegs = null;
let lastPc = -1, lastPath = null, lastBpKey = '';
let timer = null;
const LOG_MAX = 4000;
const D = { from: 0, limit: 600, total: 0, loaded: false };

/* ---------------- 工具 ---------------- */
function hint(msg, kind) {
  const h = $('hint');
  h.textContent = msg || '';
  h.className = 'hint ' + (kind || '');
  if (msg) setTimeout(() => { if (h.textContent === msg) h.textContent = ''; }, 4000);
}
function appendLog(el, lines, cls) {
  if (!lines || !lines.length) return;
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
  const frag = document.createDocumentFragment();
  for (const s of lines) {
    const d = document.createElement('div');
    d.textContent = s;
    if (cls && cls(s)) d.className = cls(s);
    frag.appendChild(d);
  }
  el.appendChild(frag);
  while (el.childElementCount > LOG_MAX) el.removeChild(el.firstChild);
  if (atBottom) el.scrollTop = el.scrollHeight;
}
const traceCls = (s) => (/^\s*\d+/.test(s) && /\bHALT\b/.test(s)) ? 'fail' : null;
const boardCls = (s) => /PASS|✔|OK\b/.test(s) ? 'pass' : (/FAIL|✘|ERROR|E \(\d+\)/.test(s) ? 'fail' : (/\[EVT|EVT /.test(s) ? 'evt' : null));

/* ---------------- 反汇编 ---------------- */
async function loadDisasm(from) {
  const r = await jget(`/api/disasm?from=${Math.max(0, from | 0)}&limit=${D.limit}`);
  if (r.err) { $('codeInfo').textContent = r.err; D.loaded = false; $('disasm').innerHTML = ''; return; }
  D.from = r.from; D.total = r.code_count; D.loaded = true;

  const frag = document.createDocumentFragment();
  for (const line of r.ins) {
    const m = line.match(/^(\d+):\s*(\S+)\s*(.*)$/);
    const ix = m ? +m[1] : -1;
    const op = m ? m[2] : '';
    const ar = m ? m[3] : line;
    const d = document.createElement('div');
    d.className = 'l';
    d.dataset.pc = ix;
    d.innerHTML = `<span class="ix">${ix}</span><span class="op">${esc(op)}</span><span class="ar">${esc(ar)}</span>`;
    d.title = '点击切换断点';
    d.onclick = () => toggleBp(ix);
    frag.appendChild(d);
  }
  const box = $('disasm');
  box.innerHTML = '';
  box.appendChild(frag);

  const end = Math.min(D.from + D.limit, D.total) - 1;
  $('codeInfo').textContent = `${D.from}–${end} / ${D.total} 条${D.total > D.limit ? '（⟨ ⟩ 翻页）' : ''}`;

  const c = $('consts'); c.innerHTML = (r.consts || []).map(s => `<div>${esc(s)}</div>`).join('') || '<div class="dim">(无)</div>';
  const f = $('funcs'); f.innerHTML = (r.funcs || []).map(s => `<div>${esc(s)}</div>`).join('') || '<div class="dim">(无)</div>';

  lastPc = -1; lastBpKey = '';
  refreshMarks();
}
function pageStep(dir) {
  if (!D.loaded) return;
  const to = D.from + dir * D.limit;
  if (to < 0 || to >= D.total) return;
  loadDisasm(to);
}
/* 断点/当前 pc 标记（只动 class，不重绘内容） */
function refreshMarks() {
  if (!D.loaded || !cur) return;
  const bpKey = (cur.bps || []).join(',') + '|' + cur.pc + '|' + cur.running;
  if (bpKey === lastBpKey && cur.pc === lastPc) return;
  lastBpKey = bpKey; lastPc = cur.pc;

  const bps = new Set(cur.bps || []);
  const box = $('disasm');
  for (const el of box.children) {
    const pc = +el.dataset.pc;
    el.classList.toggle('bp', bps.has(pc));
    el.classList.toggle('cur', !cur.running && cur.started && pc === cur.pc);
  }
  if ($('followPc').checked) {
    const curEl = box.querySelector('.l.cur');
    if (curEl) {
      const r = curEl.getBoundingClientRect(), br = box.getBoundingClientRect();
      if (r.top < br.top || r.bottom > br.bottom) curEl.scrollIntoView({ block: 'center' });
    }
  }
}
async function toggleBp(pc) {
  if (pc < 0) return;
  const on = (cur && (cur.bps || []).includes(pc)) ? 0 : 1;
  await jpost('/api/bp', { pc, on });
  if (cur) {
    const s = new Set(cur.bps || []);
    on ? s.add(pc) : s.delete(pc);
    cur.bps = [...s];
  }
  lastBpKey = '';
  refreshMarks();
}

/* ---------------- 寄存器 ---------------- */
function renderRegs(regs, watches) {
  const box = $('regs');
  const filt = $('regFilter').value.trim().replace(/^r/i, '');
  const set = new Set(watches || []);
  const frag = document.createDocumentFragment();
  let shown = 0;
  for (let i = 0; i < regs.length; i++) {
    if (filt && !String(i).startsWith(filt)) continue;
    shown++;
    const v = regs[i];
    const d = document.createElement('div');
    let cls = 'rg';
    if (v !== 0) cls += ' nz';
    if (set.has(i)) cls += ' w';
    if (prevRegs && prevRegs[i] !== v) cls += ' chg';
    d.className = cls;
    d.innerHTML = `<span class="n">r${i}</span><span class="v">${esc(fmt(v))}</span>`;
    d.title = '点击切换监视（值变化打点）';
    d.onclick = () => toggleWatch(i);
    frag.appendChild(d);
  }
  box.innerHTML = '';
  box.appendChild(frag);
  $('regCount').textContent = `${shown} / ${regs.length} 个（非零高亮 · 🟡变化 · 橙框=监视）`;
}
async function toggleWatch(r) {
  const on = (cur && (cur.watches || []).includes(r)) ? 0 : 1;
  await jpost('/api/watch', { reg: r, on });
  if (cur) {
    const s = new Set(cur.watches || []);
    on ? s.add(r) : s.delete(r);
    cur.watches = [...s];
    renderRegs(cur.regs, cur.watches);
  }
  hint(on ? `监视 r${r} 已开` : `监视 r${r} 已关`);
}

/* ---------------- 状态应用 ---------------- */
function applyState(st) {
  cur = st;
  $('stNet').textContent = '已连接';
  $('stNet').className = 'dim';
  $('stPath').textContent = st.loaded ? st.path : '未载入程序';
  $('stErr').textContent = st.err || st.error_msg || '';

  // 新程序 → 重载反汇编
  if (st.loaded && st.path !== lastPath) {
    lastPath = st.path;
    loadDisasm(0);
  }

  // 状态芯片
  const cl = $('chipLoaded');
  cl.textContent = st.loaded ? '已载入' : '未载入';
  cl.className = 'chip ' + (st.loaded ? 'on' : '');
  $('chipPc').textContent = 'pc=' + (st.started ? st.pc : '—');
  $('chipSteps').textContent = 'steps=' + st.steps;
  const rr = $('chipReason');
  rr.textContent = st.stop_reason || (st.started ? '上电' : '—');
  rr.className = 'chip ' + (st.stop_reason === 'breakpoint' ? 'warn'
    : (st.error_count > 0 || st.stop_reason === 'error') ? 'err'
    : (st.stop_reason === 'halt' ? 'on' : ''));
  const ru = $('chipRun');
  ru.textContent = st.running ? 'running…' : (st.halted ? 'halted' : 'idle');
  ru.className = 'chip ' + (st.running ? 'run' : (st.halted ? 'on' : ''));

  refreshMarks();
  renderRegs(st.regs, st.watches);
  prevRegs = st.regs;

  // 输出 / 追踪 / 监视
  appendLog($('out'), st.out);
  C.out = st.out_from;
  if ($('autoTrace').checked) { appendLog($('trace'), st.trace, traceCls); C.trace = st.trace_from; }
  else C.trace = st.trace_seq;
  appendLog($('wlog'), st.wlog);
  C.wlog = st.wlog_from;

  // 真机
  $('boardState').textContent = st.board_open ? `串口已开: ${st.board_path}` : '串口未打开';
  $('boardState').className = 'dim ' + (st.board_open ? 'ok' : '');
  const bl = $('board');
  const atBottom = $('boardFollow').checked;
  appendLog(bl, st.board, boardCls);
  if (atBottom) bl.scrollTop = bl.scrollHeight;
  C.board = st.board_from;

  if (st.halted && st.pc !== undefined) hint('程序已 HALT（▶ 运行 ⟲ 重启 可再跑一遍）');
}

async function poll() {
  try {
    const st = await jget(`/api/state?out_since=${C.out}&trace_since=${C.trace}&wlog_since=${C.wlog}&board_since=${C.board}`);
    applyState(st);
  } catch (e) {
    $('stNet').textContent = '连接断开（守护进程还在吗？）';
    $('stNet').className = 'err';
  }
  clearTimeout(timer);
  timer = setTimeout(poll, (cur && cur.running) ? 220 : 800);
}

/* ---------------- 动作 ---------------- */
async function act(name, body, msg) {
  const r = await jpost('/api/' + name, body);
  if (r && r.rc !== 0 && r.rc !== undefined) {
    const m = { '-1': '参数错误', '-2': '引擎正忙（请稍候或先暂停）', '-3': '已跑完：请 ⟲ 重启',
      '-5': '尚未上电：请先 ▶ 运行', '-4': '线程创建失败', '-10': '引擎无法停止' }[String(r.rc)];
    hint(`✘ ${name} rc=${r.rc}${r.msg ? ' · ' + r.msg : (m ? ' · ' + m : '')}`, 'err');
  } else if (msg) hint(msg, 'ok');
  poll();
}
function bind(id, fn) { const el = $(id); if (el) el.onclick = fn; }

function init() {
  // 工具栏
  bind('btnRun', () => act('run', {}, '已上电整跑'));
  bind('btnRestart', () => act('restart', {}, '已重启'));
  bind('btnPause', () => act('pause', {}, '暂停请求已发出'));
  bind('btnStep', () => act('step', { n: 1 }));
  bind('btnOver', () => act('over'));
  bind('btnOut', () => act('out'));
  bind('btnCont', () => act('cont'));
  bind('btnResetBoard', () => act('board/reset', {}, '真机 HardReset 脉冲已发'));
  bind('dPrev', () => pageStep(-1));
  bind('dNext', () => pageStep(1));
  bind('btnDisasmRefresh', () => loadDisasm(D.from));
  $('regFilter').oninput = () => { if (cur) renderRegs(cur.regs, cur.watches); };

  // 载入
  bind('btnLoad', async () => {
    const p = $('manualPath').value.trim() || $('programs').value;
    if (!p) return hint('请选择或输入程序路径', 'err');
    const r = await jpost('/api/load', { path: p });
    if (r.rc === 0) { hint('已载入 ' + p, 'ok'); lastPath = null; C.out = C.trace = C.wlog = 0; $('out').innerHTML = ''; $('trace').innerHTML = ''; $('wlog').innerHTML = ''; }
    else hint('载入失败: ' + (r.msg || r.rc), 'err');
    poll();
  });
  $('manualPath').onkeydown = (e) => { if (e.key === 'Enter') $('btnLoad').click(); };

  // 程序列表
  jget('/api/programs').then(r => {
    const sel = $('programs');
    sel.innerHTML = '';
    for (const p of (r.programs || [])) {
      const o = document.createElement('option');
      o.value = p; o.textContent = p;
      sel.appendChild(o);
    }
    if (!sel.childElementCount) sel.innerHTML = '<option value="">(未找到 .mo/.kbc)</option>';
  });

  // 标签页
  document.querySelectorAll('.tab').forEach(t => t.onclick = () => {
    document.querySelectorAll('.tab').forEach(x => x.classList.toggle('active', x === t));
    document.querySelectorAll('.tabpane').forEach(p => p.classList.toggle('active', p.id === 'pane-' + t.dataset.tab));
  });
  document.querySelectorAll('[data-clear]').forEach(b => b.onclick = () => { $(b.dataset.clear).innerHTML = ''; });

  // stdin
  const sendIn = () => {
    const v = $('stdinLine').value;
    $('stdinLine').value = '';
    if (v !== '') act('input', { line: v }, '已送入输入: ' + v);
  };
  bind('btnSend', sendIn);
  $('stdinLine').onkeydown = (e) => { if (e.key === 'Enter') sendIn(); };

  // 真机
  bind('btnBoardRefresh', refreshPorts);
  bind('btnBoardOpen', async () => {
    const p = $('ports').value;
    if (!p) return hint('请先选择端口', 'err');
    const r = await jpost('/api/board/open', { path: p, baud: +$('baud').value });
    hint(r.rc === 0 ? '串口已打开 ' + p : '打开失败 rc=' + r.rc, r.rc === 0 ? 'ok' : 'err');
    poll();
  });
  bind('btnBoardClose', () => act('board/close', {}, '串口已关闭'));
  const sendBd = () => {
    const v = $('boardLine').value;
    $('boardLine').value = '';
    if (v !== '') act('board/send', { line: v }, '已发送: ' + v);
  };
  bind('btnBoardSend', sendBd);
  $('boardLine').onkeydown = (e) => { if (e.key === 'Enter') sendBd(); };

  // 键盘快捷键
  document.addEventListener('keydown', (e) => {
    if (/input|select|textarea/i.test(e.target.tagName)) return;
    if (e.key === 'F5') { e.preventDefault(); $('btnRun').click(); }
    else if (e.key === 'F10') { e.preventDefault(); $('btnStep').click(); }
    else if (e.key === 'F8') { e.preventDefault(); $('btnCont').click(); }
    else if (e.key === 'F9') { e.preventDefault(); const pc = +prompt('切换断点 pc =', cur ? cur.pc : 0); if (!isNaN(pc)) toggleBp(pc); }
  });

  refreshPorts();
  poll();
}

async function refreshPorts() {
  try {
    const r = await jget('/api/ports');
    const sel = $('ports');
    const old = sel.value;
    sel.innerHTML = '';
    for (const p of (r.ports || [])) {
      const o = document.createElement('option');
      o.value = p.path; o.textContent = `${p.path}  (${p.desc})`;
      sel.appendChild(o);
    }
    if (!sel.childElementCount) sel.innerHTML = '<option value="">(未发现 USB 串口)</option>';
    else if (old) sel.value = old;
  } catch (e) { /* ignore */ }
}

/* ---- file:// 守卫 ----
 * 直接双击 index.html 时 protocol === 'file:', 后端 API 根本连不上
 * (fetch('/api/...') 会解析成 file:///api/... 必然失败)。
 * 此时给出正确的服务入口提示, 而不是让人对着一堆「连接中…」发懵。 */
if (location.protocol === 'file:') {
  const f = document.createElement('div');
  f.setAttribute('style', 'position:fixed;inset:0;z-index:9999;display:flex;align-items:center;'
    + 'justify-content:center;background:#12141a;color:#d6dae4;'
    + 'font:14px/1.9 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;text-align:center;padding:40px;');
  const svc = `http://127.0.0.1:${location.port || '9210'}/`;
  f.innerHTML = '<div>'
    + '<div style="font-size:34px">🐞</div>'
    + '<div style="font-weight:700;font-size:17px;margin:10px 0 6px">请通过服务地址打开</div>'
    + '<div style="color:#7d8799">这个页面必须由 <b style="color:#4aa3ff">xdebugd</b> 提供，'
    + '直接双击打开（file://）无法访问后端。</div>'
    + '<div style="margin-top:18px;color:#7d8799">先在项目目录启动服务：</div>'
    + '<div style="margin-top:6px;font:13px ui-monospace,Menlo,monospace;color:#9fd0ff">./xdebugd 9210</div>'
    + '<div style="margin-top:14px;color:#7d8799">然后在浏览器打开：</div>'
    + `<div style="margin-top:6px"><a href="${svc}" style="color:#4aa3ff;font:14px ui-monospace,Menlo,monospace">${svc}</a></div>`
    + '</div>';
  document.body.appendChild(f);
} else {
  init();
}
