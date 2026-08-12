#pragma once

#include <string_view>

namespace treeminer::dashboard {

inline constexpr std::string_view kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#05090d">
  <title>TreeMiner // HashHead Console</title>
  <script>try { const t = localStorage.getItem('treeminer-theme'); if (t && t !== 'hashhead') document.documentElement.dataset.theme = t; } catch (e) {}</script>
  <style>
    :root {
      color-scheme: dark;
      --bg: #05090d;
      --surface: rgba(7, 15, 20, .88);
      --surface-hover: rgba(10, 22, 30, .92);
      --surface-strong: #091219;
      --accent: #2de1eb;
      --accent2: #70f58a;
      --line: color-mix(in srgb, var(--accent) 20%, transparent);
      --line-soft: color-mix(in srgb, var(--accent) 10%, transparent);
      --line-strong: color-mix(in srgb, var(--accent) 48%, transparent);
      --cyan: var(--accent);
      --green: var(--accent2);
      --amber: #ffc857;
      --red: #ff647c;
      --ink: #edfdfd;
      --muted: #82949b;
      --dim: #53656c;
      --mono: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
      --display: Inter, ui-sans-serif, system-ui, sans-serif;
    }
    html[data-theme="synthwave"] { --accent: #ff6ac1; --accent2: #b18cff; }
    html[data-theme="phosphor"] { --accent: #4af626; --accent2: #8dff70; }
    html[data-theme="arctic"] { --accent: #7cc4ff; --accent2: #d6ecff; }
    * { box-sizing: border-box; }
    ::selection { background: color-mix(in srgb, var(--accent) 28%, transparent); }
    body {
      margin: 0;
      min-width: 320px;
      min-height: 100vh;
      background: var(--bg) url('/assets/hashfield.webp') top center / cover fixed no-repeat;
      color: var(--ink);
      font-family: var(--mono);
      letter-spacing: 0;
    }
    body::before {
      content: "";
      position: fixed;
      inset: 0;
      z-index: -1;
      background: rgba(3, 8, 12, .76);
      pointer-events: none;
    }
    button { font: inherit; }
    :focus-visible { outline: 1px solid var(--cyan); outline-offset: 2px; }
    ::-webkit-scrollbar { width: 10px; height: 10px; }
    ::-webkit-scrollbar-thumb { background: color-mix(in srgb, var(--accent) 18%, transparent); border: 3px solid var(--bg); border-radius: 5px; }
    ::-webkit-scrollbar-track { background: transparent; }
    .shell { width: min(1440px, calc(100% - 40px)); margin: 0 auto; }
    .topbar {
      height: 66px;
      border-bottom: 1px solid var(--line);
      background: rgba(4, 10, 14, .9);
      display: flex;
      align-items: center;
      backdrop-filter: blur(6px);
    }
    .topbar .shell, .brand, .network, .actions, .inline { display: flex; align-items: center; }
    .topbar .shell { justify-content: space-between; gap: 20px; }
    .brand { gap: 12px; min-width: 0; }
    .brand-mark {
      width: 30px; height: 30px; border: 1px solid var(--cyan);
      display: grid; place-items: center; color: var(--green); font-weight: 800;
      box-shadow: inset 0 0 18px color-mix(in srgb, var(--accent) 12%, transparent);
    }
    .brand-name { font-family: var(--display); font-size: 15px; font-weight: 750; letter-spacing: .02em; }
    .brand-path { color: var(--dim); font-size: 11px; }
    .topside { display: flex; align-items: center; gap: 18px; }
    .themes { display: flex; align-items: center; gap: 7px; }
    .swatch { width: 13px; height: 13px; padding: 0; border: 1px solid rgba(255, 255, 255, .25); border-radius: 50%; background: var(--sw); cursor: pointer; opacity: .4; transition: opacity .2s ease, box-shadow .2s ease; }
    .swatch:hover { opacity: .8; }
    .swatch.active { opacity: 1; box-shadow: 0 0 10px var(--sw); }
    .network { gap: 9px; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: .09em; }
    .dot { width: 7px; height: 7px; border-radius: 50%; color: var(--green); background: currentColor; box-shadow: 0 0 14px currentColor; animation: breathe 2.8s ease-in-out infinite; }
    .dot.offline { color: var(--amber); }
    .dot.probing { color: var(--cyan); animation-duration: 1.2s; }
    @keyframes breathe { 0%, 100% { box-shadow: 0 0 5px currentColor; } 50% { box-shadow: 0 0 16px currentColor; } }
    main { padding: 28px 0 56px; animation: rise .5s ease-out both; }
    @keyframes rise { from { opacity: 0; transform: translateY(8px); } }
    .alert {
      display: none; align-items: center; justify-content: space-between; gap: 20px;
      padding: 13px 16px; margin-bottom: 22px; border: 1px solid rgba(255, 200, 87, .5);
      border-left: 3px solid var(--amber);
      background: rgba(40, 29, 5, .9); color: #ffe6a5; font-size: 12px;
    }
    .alert.show { display: flex; animation: slidein .35s ease-out; }
    @keyframes slidein { from { opacity: 0; transform: translateY(-6px); } }
    .alert strong { color: var(--amber); }
    .eyebrow { color: var(--cyan); font-size: 11px; text-transform: uppercase; letter-spacing: .09em; margin-bottom: 12px; }
    .eyebrow .index { color: var(--dim); }
    .hero {
      min-height: 210px; display: grid; grid-template-columns: minmax(0, 1.5fr) minmax(300px, .7fr);
      gap: 32px; align-items: end; padding: 22px 0 30px; border-bottom: 1px solid var(--line);
    }
    .hero h1 { margin: 0; font: 750 clamp(27px, 4vw, 54px)/1 var(--display); letter-spacing: -.01em; }
    .hero h1 span { color: var(--cyan); }
    .hero-meta { margin-top: 14px; color: var(--muted); font-size: 12px; overflow-wrap: anywhere; word-break: break-all; }
    .rate-label { color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: .09em; }
    .rate { margin-top: 7px; color: var(--green); font: 720 clamp(34px, 5vw, 68px)/.95 var(--display); white-space: nowrap; font-variant-numeric: tabular-nums; text-shadow: 0 0 32px color-mix(in srgb, var(--accent2) 22%, transparent); }
    .rate-unit { color: var(--muted); font: 500 14px/1 var(--mono); text-shadow: none; }
    .split-rate { margin-top: 13px; color: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
    .spark { position: relative; margin-top: 16px; }
    .spark svg { display: block; width: 100%; height: 64px; overflow: visible; cursor: crosshair; }
    #spark-line { fill: none; stroke: var(--accent); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
    .spark .g0 { stop-color: var(--accent); stop-opacity: .2; }
    .spark .g1 { stop-color: var(--accent); stop-opacity: 0; }
    #spark-cursor { stroke: color-mix(in srgb, var(--accent) 45%, transparent); stroke-width: 1; }
    #spark-hover { fill: var(--accent); stroke: var(--surface-strong); stroke-width: 2; }
    #spark-dot { fill: var(--accent); }
    #spark-finds circle { fill: var(--accent2); stroke: var(--surface-strong); stroke-width: 1.5; }
    .find-key { color: var(--accent2); }
    .spark-caption { display: flex; justify-content: space-between; gap: 12px; margin-top: 7px; color: var(--dim); font-size: 9px; text-transform: uppercase; letter-spacing: .09em; font-variant-numeric: tabular-nums; }
    .spark-tip {
      position: absolute; top: -8px; transform: translate(-50%, -100%);
      padding: 7px 9px; border: 1px solid var(--line-strong); background: var(--surface-strong);
      font-size: 10px; line-height: 1.6; color: var(--muted); white-space: nowrap; pointer-events: none;
      font-variant-numeric: tabular-nums; box-shadow: 0 6px 24px rgba(0, 0, 0, .5); z-index: 2;
    }
    .spark-tip strong { color: var(--ink); font-weight: 600; }
    .spark-tip .t { color: var(--dim); }
    .grid { display: grid; gap: 1px; background: var(--line); border: 1px solid var(--line); margin-top: 24px; }
    .metrics { grid-template-columns: repeat(6, minmax(0, 1fr)); }
    .metric { position: relative; min-height: 112px; padding: 17px; background: var(--surface); transition: background .25s ease; }
    .metric::before { content: ""; position: absolute; top: 0; left: 0; right: 0; height: 2px; background: linear-gradient(90deg, var(--cyan), transparent 70%); opacity: 0; transition: opacity .25s ease; }
    .metric:hover { background: var(--surface-hover); }
    .metric:hover::before { opacity: .65; }
    .metric-label { color: var(--muted); font-size: 10px; text-transform: uppercase; letter-spacing: .09em; }
    .metric-value { margin-top: 18px; font: 700 26px/1 var(--display); font-variant-numeric: tabular-nums; }
    .metric-value.warn { color: var(--amber); }
    .metric-detail { margin-top: 8px; color: var(--dim); font-size: 10px; }
    .tick { animation: tick .8s ease-out; }
    @keyframes tick { 0% { color: var(--cyan); text-shadow: 0 0 20px color-mix(in srgb, var(--accent) 60%, transparent); } }
    .section { margin-top: 34px; }
    .section-head { display: flex; justify-content: space-between; align-items: end; gap: 18px; margin-bottom: 12px; }
    .section-head h2 { margin: 0; font: 680 18px/1.1 var(--display); }
    .section-head p { margin: 5px 0 0; color: var(--dim); font-size: 10px; }
    .actions { gap: 8px; }
    .command {
      min-height: 34px; padding: 0 12px; border: 1px solid var(--line-strong); border-radius: 3px;
      background: rgba(5, 15, 19, .88); color: var(--cyan); cursor: pointer; font-size: 10px;
      text-transform: uppercase; letter-spacing: .09em;
      transition: background .2s ease, border-color .2s ease, box-shadow .2s ease, transform .15s ease;
    }
    .command:hover, .command:focus-visible { background: color-mix(in srgb, var(--accent) 10%, transparent); outline: none; border-color: var(--cyan); box-shadow: 0 0 18px color-mix(in srgb, var(--accent) 18%, transparent); }
    .command:active { transform: translateY(1px); }
    .ops { grid-template-columns: 1.35fr .65fr; }
    .panel { background: var(--surface); min-width: 0; }
    .panel-title { padding: 14px 16px; border-bottom: 1px solid var(--line); color: var(--cyan); font-size: 10px; text-transform: uppercase; letter-spacing: .09em; }
    table { width: 100%; border-collapse: collapse; table-layout: fixed; }
    th, td { padding: 14px 16px; border-bottom: 1px solid var(--line-soft); text-align: left; font-size: 11px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    th { color: var(--dim); font-weight: 500; text-transform: uppercase; letter-spacing: .09em; font-size: 9px; }
    td { color: var(--muted); font-variant-numeric: tabular-nums; }
    td.primary { color: var(--ink); }
    td.signal { color: var(--green); }
    tbody tr { transition: background .15s ease; }
    tbody tr:hover { background: color-mix(in srgb, var(--accent) 4%, transparent); }
    .delivery { padding: 5px 16px 15px; }
    .delivery-row { display: flex; align-items: center; justify-content: space-between; gap: 20px; padding: 13px 0; border-bottom: 1px solid var(--line-soft); font-size: 11px; }
    .delivery-row:last-child { border-bottom: 0; }
    .delivery-row span:first-child { color: var(--muted); }
    .delivery-row strong { color: var(--ink); font-weight: 600; text-align: right; font-variant-numeric: tabular-nums; }
    .queue { color: var(--amber) !important; }
    .profile { grid-template-columns: repeat(4, minmax(0, 1fr)); }
    .profile-item { position: relative; min-height: 94px; background: var(--surface); padding: 15px; transition: background .25s ease; }
    .profile-item:hover { background: var(--surface-hover); }
    .profile-key { color: var(--dim); text-transform: uppercase; letter-spacing: .09em; font-size: 9px; }
    .profile-value { margin-top: 12px; color: var(--ink); font-size: 11px; overflow-wrap: anywhere; }
    footer { border-top: 1px solid var(--line); padding: 18px 0 26px; color: var(--dim); font-size: 9px; text-transform: uppercase; letter-spacing: .09em; }
    footer .shell { display: flex; justify-content: space-between; gap: 18px; }
    .stale { color: var(--amber); }
    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after { animation: none !important; transition: none !important; }
    }
    @media (max-width: 1000px) {
      .metrics { grid-template-columns: repeat(3, 1fr); }
      .ops { grid-template-columns: 1fr; }
      .profile { grid-template-columns: repeat(2, 1fr); }
    }
    @media (max-width: 680px) {
      .shell { width: min(100% - 24px, 1440px); }
      .brand-path { display: none; }
      #network-label { display: none; }
      .hero { grid-template-columns: 1fr; min-height: 0; align-items: start; }
      .metrics { grid-template-columns: repeat(2, 1fr); }
      .metric { min-height: 96px; }
      .profile { grid-template-columns: 1fr; }
      .section-head { align-items: start; flex-direction: column; }
      th:nth-child(3), td:nth-child(3), th:nth-child(5), td:nth-child(5) { display: none; }
      footer .shell { flex-direction: column; }
      .alert { align-items: start; flex-direction: column; }
    }
  </style>
</head>
<body>
  <header class="topbar">
    <div class="shell">
      <div class="brand"><div class="brand-mark">T</div><div><div class="brand-name">HASHHEAD // TREEMINER</div><div class="brand-path">LOCAL_OPS::42069</div></div></div>
      <div class="topside">
        <div class="themes" role="group" aria-label="Console theme">
          <button class="swatch active" data-theme-pick="hashhead" style="--sw:#2de1eb" title="Theme: HashHead" aria-label="Theme: HashHead" type="button"></button>
          <button class="swatch" data-theme-pick="synthwave" style="--sw:#ff6ac1" title="Theme: Synthwave" aria-label="Theme: Synthwave" type="button"></button>
          <button class="swatch" data-theme-pick="phosphor" style="--sw:#4af626" title="Theme: Phosphor" aria-label="Theme: Phosphor" type="button"></button>
          <button class="swatch" data-theme-pick="arctic" style="--sw:#7cc4ff" title="Theme: Arctic" aria-label="Theme: Arctic" type="button"></button>
        </div>
        <div class="network"><span id="network-dot" class="dot"></span><span id="network-label" aria-live="polite">connecting</span></div>
      </div>
    </div>
  </header>
  <main class="shell">
    <div id="outage" class="alert"><span><strong>UPSTREAM OFFLINE</strong> // Mining continues. <span id="outage-copy">Finds are held locally.</span></span><span id="outage-queue">0 secured</span></div>
    <section class="hero">
      <div>
        <div class="eyebrow"><span class="index">[01]</span> :: ACTIVE_RIG</div>
        <h1 id="rig-name">TREE<span>MINER</span></h1>
        <div class="hero-meta"><span id="address">0x...</span> // <span id="machine">waiting for telemetry</span> // <span id="console-url">waiting for console url</span></div>
      </div>
      <div>
        <div class="rate-label">Combined throughput</div>
        <div class="rate"><span id="total-rate">--</span> <span class="rate-unit">kH/s</span></div>
        <div class="split-rate"><span id="gpu-rate">GPU --</span> // <span id="cpu-rate">CPU --</span></div>
        <div class="spark">
          <svg id="spark-svg" role="img" aria-label="Combined hashrate, last 10 minutes">
            <defs>
              <linearGradient id="spark-fill" x1="0" y1="0" x2="0" y2="1">
                <stop class="g0" offset="0"/>
                <stop class="g1" offset="1"/>
              </linearGradient>
            </defs>
            <path id="spark-area" fill="url(#spark-fill)" d=""/>
            <path id="spark-line" d=""/>
            <line id="spark-cursor" y1="0" y2="0" visibility="hidden"/>
            <g id="spark-finds"></g>
            <circle id="spark-hover" r="3.5" visibility="hidden"/>
            <circle id="spark-dot" r="2.5" visibility="hidden"/>
          </svg>
          <div id="spark-tip" class="spark-tip" hidden></div>
          <div class="spark-caption"><span>throughput // last 10 min // <span class="find-key">&#9679;</span> find</span><span id="spark-range">collecting samples...</span></div>
        </div>
      </div>
    </section>

    <section class="grid metrics" aria-label="Mining metrics">
      <div class="metric"><div class="metric-label">Difficulty</div><div id="difficulty" class="metric-value">--</div><div class="metric-detail">network target</div></div>
      <div class="metric"><div class="metric-label">XNM Finds</div><div id="xnm" class="metric-value">--</div><div class="metric-detail">this session</div></div>
      <div class="metric"><div class="metric-label">XUNI Finds</div><div id="xuni" class="metric-value">--</div><div class="metric-detail">this session</div></div>
      <div class="metric"><div class="metric-label">Q_XNM</div><div id="q-xnm" class="metric-value">--</div><div class="metric-detail">secured locally</div></div>
      <div class="metric"><div class="metric-label">Q_XUNI</div><div id="q-xuni" class="metric-value">--</div><div class="metric-detail">secured locally</div></div>
      <div class="metric"><div class="metric-label">Uptime</div><div id="uptime" class="metric-value">--</div><div class="metric-detail">current process</div></div>
    </section>

    <section class="section">
      <div class="section-head"><div><div class="eyebrow"><span class="index">[02]</span> :: ENGINE</div><h2>Compute & delivery</h2><p>Live worker streams and submission state</p></div></div>
      <div class="grid ops">
        <div class="panel">
          <div class="panel-title">CUDA stream telemetry</div>
          <table><thead><tr><th>Device</th><th>Stream</th><th>Memory</th><th>Hashrate</th><th>Work</th></tr></thead><tbody id="gpu-rows"><tr><td colspan="5">Awaiting telemetry...</td></tr></tbody></table>
        </div>
        <div class="panel">
          <div class="panel-title">Delivery channel</div>
          <div class="delivery">
            <div class="delivery-row"><span>Network</span><strong id="delivery-network">--</strong></div>
            <div class="delivery-row"><span>Last submission</span><strong id="last-submission">--</strong></div>
            <div class="delivery-row"><span>Secured queue</span><strong id="queue-total" class="queue">--</strong></div>
            <div class="delivery-row"><span>CPU sidecar</span><strong id="cpu-workers">--</strong></div>
            <div class="delivery-row"><span>CUDA streams</span><strong id="stream-count">--</strong></div>
          </div>
        </div>
      </div>
    </section>

    <section class="section">
      <div class="section-head">
        <div><div class="eyebrow"><span class="index">[03]</span> :: RIG_PROFILE</div><h2>Runtime profile</h2><p>Shareable operational snapshot; no secrets included</p></div>
        <div class="actions"><button id="export" class="command" type="button">Export snapshot</button></div>
      </div>
      <div class="grid profile">
        <div class="profile-item"><div class="profile-key">Miner address</div><div id="profile-address" class="profile-value">--</div></div>
        <div class="profile-item"><div class="profile-key">Machine ID</div><div id="profile-machine" class="profile-value">--</div></div>
        <div class="profile-item"><div class="profile-key">GPU plan</div><div id="profile-gpu" class="profile-value">--</div></div>
        <div class="profile-item"><div class="profile-key">CPU plan</div><div id="profile-cpu" class="profile-value">--</div></div>
        <div class="profile-item"><div class="profile-key">Open at</div><div id="profile-console" class="profile-value">--</div></div>
      </div>
    </section>
  </main>
  <footer><div class="shell"><span>HASHHEAD // TREECITYWES // LOCAL MINING INFRASTRUCTURE</span><span id="freshness">telemetry pending</span></div></footer>
  <script>
    let snapshot = null;
    const $ = (id) => document.getElementById(id);
    const TICK_IDS = new Set(['difficulty', 'xnm', 'xuni', 'q-xnm', 'q-xuni', 'queue-total', 'last-submission']);
    const text = (id, value) => {
      const el = $(id), next = String(value);
      if (el.textContent === next) return;
      const flash = TICK_IDS.has(id) && el.textContent !== '--';
      el.textContent = next;
      if (flash) { el.classList.remove('tick'); void el.offsetWidth; el.classList.add('tick'); }
    };
    const kh = (value) => (Number(value || 0) / 1000).toFixed(2);
    const compact = (value) => Number(value || 0).toLocaleString();
    const duration = (seconds) => {
      const s = Math.max(0, Number(seconds || 0));
      const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
      return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`;
    };
    const escapeHtml = (value) => String(value).replace(/[&<>'"]/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));

    // Rolling hashrate history, ~10 min at one sample per 2.5s poll.
    const SPARK_MAX = 240;
    const sparkBuf = [];
    let sparkPts = [];
    let prevFinds = null;
    function pushSample(engine, finds) {
      sparkBuf.push({ t: Date.now(), total: Number(engine.total_hashrate || 0), gpu: Number(engine.gpu_hashrate || 0), cpu: Number(engine.cpu_hashrate || 0), finds });
      if (sparkBuf.length > SPARK_MAX) sparkBuf.shift();
    }
    function drawSpark() {
      const svg = $('spark-svg');
      const w = svg.clientWidth, h = svg.clientHeight;
      if (!w || !h || sparkBuf.length < 2) return;
      svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
      const pad = 4;
      const t0 = sparkBuf[0].t, span = Math.max(1, sparkBuf[sparkBuf.length - 1].t - t0);
      let lo = Infinity, hi = -Infinity;
      for (const s of sparkBuf) { lo = Math.min(lo, s.total); hi = Math.max(hi, s.total); }
      if (hi - lo < hi * .02 + 1) { const mid = (hi + lo) / 2, half = Math.max(mid * .02, 500); lo = mid - half; hi = mid + half; }
      const X = (t) => pad + (w - 2 * pad) * (t - t0) / span;
      const Y = (v) => h - pad - (h - 2 * pad) * (v - lo) / (hi - lo);
      sparkPts = sparkBuf.map((s) => ({ x: X(s.t), y: Y(s.total), s }));
      const d = sparkPts.map((p, i) => `${i ? 'L' : 'M'}${p.x.toFixed(1)},${p.y.toFixed(1)}`).join('');
      $('spark-line').setAttribute('d', d);
      $('spark-area').setAttribute('d', `${d}L${sparkPts[sparkPts.length - 1].x.toFixed(1)},${h}L${sparkPts[0].x.toFixed(1)},${h}Z`);
      const last = sparkPts[sparkPts.length - 1];
      const dot = $('spark-dot');
      dot.setAttribute('cx', last.x); dot.setAttribute('cy', last.y); dot.setAttribute('visibility', 'visible');
      $('spark-finds').innerHTML = sparkPts.filter((p) => p.s.finds > 0).map((p) => `<circle cx="${p.x.toFixed(1)}" cy="${p.y.toFixed(1)}" r="2.5"/>`).join('');
      text('spark-range', `${kh(lo)}–${kh(hi)} kH/s`);
    }
    (() => {
      const svg = $('spark-svg'), tip = $('spark-tip'), cursor = $('spark-cursor'), hover = $('spark-hover');
      svg.addEventListener('pointermove', (event) => {
        if (!sparkPts.length) return;
        const x = event.clientX - svg.getBoundingClientRect().left;
        let best = sparkPts[0];
        for (const p of sparkPts) if (Math.abs(p.x - x) < Math.abs(best.x - x)) best = p;
        cursor.setAttribute('x1', best.x); cursor.setAttribute('x2', best.x);
        cursor.setAttribute('y2', svg.clientHeight);
        cursor.setAttribute('visibility', 'visible');
        hover.setAttribute('cx', best.x); hover.setAttribute('cy', best.y);
        hover.setAttribute('visibility', 'visible');
        const findLine = best.s.finds > 0 ? `<br><span class="find-key">&#9679;</span> +${best.s.finds} find${best.s.finds > 1 ? 's' : ''}` : '';
        tip.innerHTML = `<span class="t">${new Date(best.s.t).toLocaleTimeString()}</span><br><strong>${kh(best.s.total)} kH/s</strong><br>GPU ${kh(best.s.gpu)} // CPU ${kh(best.s.cpu)}${findLine}`;
        tip.hidden = false;
        tip.style.left = `${Math.min(Math.max(best.x, 70), svg.clientWidth - 70)}px`;
      });
      svg.addEventListener('pointerleave', () => {
        tip.hidden = true;
        cursor.setAttribute('visibility', 'hidden');
        hover.setAttribute('visibility', 'hidden');
      });
      window.addEventListener('resize', drawSpark);
    })();

    function render(data) {
      snapshot = data;
      const { identity, engine, finds, delivery, gpus } = data;
      text('rig-name', identity.name.toUpperCase());
      text('address', identity.address);
      text('machine', identity.machine_id);
      text('total-rate', kh(engine.total_hashrate));
      text('gpu-rate', `GPU ${kh(engine.gpu_hashrate)} kH/s`);
      text('cpu-rate', `CPU ${kh(engine.cpu_hashrate)} kH/s`);
      text('difficulty', compact(engine.difficulty));
      text('xnm', compact(finds.xnm)); text('xuni', compact(finds.xuni));
      text('q-xnm', compact(delivery.queued_xnm)); text('q-xuni', compact(delivery.queued_xuni));
      $('q-xnm').classList.toggle('warn', Number(delivery.queued_xnm || 0) > 0);
      $('q-xuni').classList.toggle('warn', Number(delivery.queued_xuni || 0) > 0);
      text('uptime', duration(engine.uptime_seconds));
      text('delivery-network', delivery.network.toUpperCase());
      text('last-submission', delivery.last_submission.toUpperCase());
      text('queue-total', `${compact(delivery.queued_total)} finds`);
      text('cpu-workers', `${engine.cpu_workers} workers`);
      text('stream-count', engine.cuda_streams);
      text('profile-address', identity.address); text('profile-machine', identity.machine_id);
      text('profile-gpu', `${engine.gpu_devices} GPU // ${engine.cuda_streams} streams`);
      text('profile-cpu', engine.cpu_workers ? `${engine.cpu_workers} workers // ${kh(engine.cpu_hashrate)} kH/s` : 'disabled');
      const consoleUrl = (data.console && data.console.open) || '';
      text('console-url', consoleUrl || 'waiting for console url');
      text('profile-console', consoleUrl || '--');
      text('network-label', `network ${delivery.network}`);
      $('network-dot').className = `dot ${delivery.network}`;
      const offline = delivery.network !== 'online';
      $('outage').classList.toggle('show', offline);
      text('outage-copy', delivery.network === 'probing' ? 'Testing the upstream connection; finds remain local.' : 'Mining continues; finds remain local until delivery resumes.');
      text('outage-queue', `${compact(delivery.queued_total)} secured`);
      $('gpu-rows').innerHTML = gpus.length ? gpus.map((gpu) => `<tr><td class="primary">#${gpu.index} ${escapeHtml(gpu.name || '')}</td><td>${gpu.stream + 1}</td><td>${Number(gpu.memory_used_percent || 0).toFixed(1)}%</td><td class="signal">${kh(gpu.hashrate)} kH/s</td><td>${compact(gpu.hash_count)}</td></tr>`).join('') : '<tr><td colspan="5">No active GPU streams</td></tr>';
      const newFinds = prevFinds ? Math.max(0, Number(finds.xnm || 0) - prevFinds.xnm) + Math.max(0, Number(finds.xuni || 0) - prevFinds.xuni) : 0;
      prevFinds = { xnm: Number(finds.xnm || 0), xuni: Number(finds.xuni || 0) };
      pushSample(engine, newFinds);
      drawSpark();
      text('freshness', `telemetry live // ${new Date().toLocaleTimeString()}`);
      $('freshness').classList.remove('stale');
    }
    async function refresh() {
      try {
        const response = await fetch('/api/rig', { cache: 'no-store' });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        render(await response.json());
      } catch (error) {
        text('network-label', 'local telemetry unavailable');
        $('network-dot').className = 'dot offline';
        text('freshness', 'telemetry stale // retrying');
        $('freshness').classList.add('stale');
      }
    }
    $('export').addEventListener('click', () => {
      if (!snapshot) return;
      const blob = new Blob([JSON.stringify(snapshot, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob), link = document.createElement('a');
      link.href = url; link.download = `treeminer-${snapshot.identity.machine_id}-snapshot.json`; link.click();
      URL.revokeObjectURL(url);
    });
    const swatches = document.querySelectorAll('.swatch');
    function applyTheme(name) {
      if (name === 'hashhead') delete document.documentElement.dataset.theme;
      else document.documentElement.dataset.theme = name;
      try { localStorage.setItem('treeminer-theme', name); } catch (e) {}
      swatches.forEach((b) => b.classList.toggle('active', b.dataset.themePick === name));
      drawSpark();
    }
    swatches.forEach((b) => b.addEventListener('click', () => applyTheme(b.dataset.themePick)));
    try { applyTheme(localStorage.getItem('treeminer-theme') || 'hashhead'); } catch (e) {}
    refresh(); setInterval(refresh, 2500);
  </script>
</body>
</html>)HTML";

} // namespace treeminer::dashboard
