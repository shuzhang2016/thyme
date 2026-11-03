#!/usr/bin/env python3
"""Render an eval_chains JSON report into a self-contained offline HTML report.

Reads the JSON produced by `eval_chains` (single-pipeline: percentile
summaries + per-sample time series + /proc resource sampling) and writes a
single HTML file with tables and ECharts time-series charts in the same visual
style as the manager's chain-eval reports. ECharts is inlined from the bundled
`echarts.min.js` next to this script, so the output works offline.

Usage:
  python3 render_eval.py --in eval.json --out chain_eval_report.html

Exit codes:
  0  rendered OK
  2  usage / missing input
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _ns_to_us(v) -> float:
    try:
        return round(float(v) / 1000.0, 1)
    except (TypeError, ValueError):
        return 0.0


def _load(path: Path) -> dict:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def _normalize_resources(res) -> dict[str, list]:
    """Return {role: [(t_ms, cpu_jif, rss_kb)]} for both resource shapes."""
    out: dict[str, list] = {}
    if isinstance(res, dict):
        for role, pts in res.items():
            out[role] = [(p[0], p[1], p[2]) for p in pts]
    elif isinstance(res, list):
        out["self"] = [(p[0], p[1], p[2]) for p in res]
    return out


def _cpu_series(pts):
    """Convert cumulative cpu_jif into per-sample CPU% and RSS MB."""
    cpu, rss = [], []
    prev_t = prev_j = None
    for (t, j, r) in pts:
        if prev_t is not None and t > prev_t:
            cpu_pct = (j - prev_j) * 1000.0 / (t - prev_t)  # jiffies(1/100s) -> %
            cpu.append([t, round(cpu_pct, 2)])
        prev_t, prev_j = t, j
        rss.append([t, round(r / 1024.0, 2)])
    return cpu, rss


def _mean(vals):
    if not vals:
        return 0.0
    return round(sum(vals) / len(vals), 2)


def _conclusion(doc: dict) -> str:
    e2e = doc.get("e2e") or {}
    p50 = _ns_to_us(e2e.get("p50", 0))
    p95 = _ns_to_us(e2e.get("p95", 0))
    broken = e2e.get("broken", 0)
    n = e2e.get("n", 0)
    chain = " → ".join(doc.get("chain", [])) or "chain"
    parts = [f"Chain <b>{chain}</b>: {n} completed frames, <b>{broken}</b> broken "
             f"(dropped/gate-rejected), E2E p50 = <b>{p50} µs</b>, p95 = <b>{p95} µs</b>."]
    if broken:
        parts.append("Non-zero broken frames indicate the chain drops under this "
                     "load — investigate latest-slot overwrites, ts-gate drops, or "
                     "pool exhaustion.")
    else:
        parts.append("No broken frames: the chain is lossless under this load.")
    edges = doc.get("edges") or []
    if edges:
        worst = max(edges, key=lambda e: e.get("lat_p95", 0) or 0)
        parts.append(
            f"Highest per-edge p95 latency is <b>{worst['channel']}</b> "
            f"({_ns_to_us(worst.get('lat_p95', 0))} µs).")
    return "<br>".join(parts)


def render(doc: dict, echarts_js: str) -> str:
    title = "thyme pipeline performance report"
    sub = (
        f"{doc.get('mode', 'inproc')} · load={doc.get('load', 'baseline')} · "
        f"{doc.get('seconds', 0)}s · chain: {', '.join(doc.get('chain', []))}"
    )

    nodes = doc.get("nodes") or []
    edges = doc.get("edges") or []
    e2e = doc.get("e2e") or {}

    # ---- node summary table (latency + work, ns -> us) ----
    node_rows = []
    for n in nodes:
        node_rows.append(
            "<tr>"
            f"<td>{n.get('node', '?')}</td>"
            f"<td>{n.get('lat_n', 0)}</td>"
            f"<td>{_ns_to_us(n.get('lat_p50', 0))}</td>"
            f"<td>{_ns_to_us(n.get('lat_p95', 0))}</td>"
            f"<td>{_ns_to_us(n.get('lat_p99', 0))}</td>"
            f"<td>{_ns_to_us(n.get('lat_max', 0))}</td>"
            f"<td>{n.get('work_n', 0)}</td>"
            f"<td>{_ns_to_us(n.get('work_p50', 0))}</td>"
            f"<td>{_ns_to_us(n.get('work_p95', 0))}</td>"
            f"<td>{_ns_to_us(n.get('work_max', 0))}</td>"
            f"<td>{n.get('idle', 0)}</td>"
            "</tr>"
        )
    node_table = (
        "<table><tr><th>node</th><th>lat n</th><th>lat p50 µs</th>"
        "<th>lat p95 µs</th><th>lat p99 µs</th><th>lat max µs</th>"
        "<th>work n</th><th>work p50 µs</th><th>work p95 µs</th>"
        "<th>work max µs</th><th>idle</th></tr>" + "".join(node_rows) + "</table>"
    )

    # ---- edge summary table ----
    edge_rows = []
    for e in edges:
        edge_rows.append(
            "<tr>"
            f"<td>{e.get('channel', '?')}</td>"
            f"<td>{e.get('count', 0)}</td>"
            f"<td>{e.get('rate', 0)}</td>"
            f"<td>{_ns_to_us(e.get('lat_p50', 0))}</td>"
            f"<td>{_ns_to_us(e.get('lat_p95', 0))}</td>"
            f"<td>{_ns_to_us(e.get('lat_p99', 0))}</td>"
            f"<td>{_ns_to_us(e.get('lat_max', 0))}</td>"
            f"<td>{e.get('jit_std', 0)}</td>"
            "</tr>"
        )
    edge_table = (
        "<table><tr><th>channel</th><th>count</th><th>rate/s</th>"
        "<th>p50 µs</th><th>p95 µs</th><th>p99 µs</th><th>max µs</th>"
        "<th>jitter std</th></tr>" + "".join(edge_rows) + "</table>"
    )

    # ---- e2e summary table ----
    e2e_table = (
        "<table><tr><th>frames</th><th>broken</th><th>p50 µs</th>"
        "<th>p95 µs</th><th>p99 µs</th><th>max µs</th></tr>"
        "<tr>"
        f"<td>{e2e.get('n', 0)}</td>"
        f"<td>{e2e.get('broken', 0)}</td>"
        f"<td>{_ns_to_us(e2e.get('p50', 0))}</td>"
        f"<td>{_ns_to_us(e2e.get('p95', 0))}</td>"
        f"<td>{_ns_to_us(e2e.get('p99', 0))}</td>"
        f"<td>{_ns_to_us(e2e.get('max', 0))}</td>"
        "</tr></table>"
    )

    # ---- resources table (mean CPU% + RSS range) ----
    resources = _normalize_resources(doc.get("resources"))
    res_rows = []
    for role, pts in resources.items():
        cpu, rss = _cpu_series(pts)
        cpu_mean = _mean([p[1] for p in cpu])
        rss_vals = [p[1] for p in rss]
        rss_range = f"{min(rss_vals):.1f}-{max(rss_vals):.1f}" if rss_vals else "0"
        res_rows.append(
            "<tr>"
            f"<td>{role}</td>"
            f"<td>{cpu_mean}</td>"
            f"<td>{rss_range}</td>"
            "</tr>"
        )
    res_table = (
        "<table><tr><th>role</th><th>CPU % (mean)</th><th>RSS MB (range)</th></tr>"
        + "".join(res_rows) + "</table>"
    )

    payload = json.dumps(
        {
            "node_series": doc.get("node_series") or {},
            "edge_series": doc.get("edge_series") or {},
            "work_series": doc.get("work_series") or {},
            "e2e_series": (e2e.get("series") or []),
            "resources": resources,
        }
    )

    html = HTML_TEMPLATE.replace("__TITLE__", title)
    html = html.replace("__SUB__", sub)
    html = html.replace("__NODE_TABLE__", node_table)
    html = html.replace("__EDGE_TABLE__", edge_table)
    html = html.replace("__E2E_TABLE__", e2e_table)
    html = html.replace("__RES_TABLE__", res_table)
    html = html.replace("__CONCLUSION__", _conclusion(doc))
    html = html.replace("__DATA__", payload)
    html = html.replace("__ECHARTS__", echarts_js)
    return html


HTML_TEMPLATE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>
:root{--cyc:#2563eb}
body{font-family:-apple-system,"Segoe UI",Roboto,sans-serif;margin:32px;color:#1f2937;max-width:1200px;line-height:1.6}
h1{font-size:22px;margin-bottom:2px}
.sub{color:#6b7280;font-size:13px}
h2{font-size:17px;margin-top:36px;border-bottom:2px solid #d1d5db;padding-bottom:5px}
.chart{width:100%;height:380px;margin:12px 0;border:1px solid #e5e7eb;border-radius:8px}
.explain{background:#f9fafb;border-left:4px solid #93c5fd;padding:10px 14px;margin:10px 0;font-size:13px}
.final{background:#eff6ff;border:2px solid #2563eb;border-radius:8px;padding:16px 20px;margin-top:14px;font-size:14px}
table{border-collapse:collapse;width:100%;font-size:12.5px;margin:10px 0}
th,td{border:1px solid #e5e7eb;padding:5px 8px;text-align:right;white-space:nowrap}
th{background:#f9fafb}
td:first-child,th:first-child{text-align:left}
.toggle{font-size:12px;color:#6b7280;margin:2px 0}
</style></head><body>
<h1>__TITLE__</h1>
<div class="sub">__SUB__</div>

<div class="explain"><b>How to read:</b> the chain's latency is measured per hop (producer publish → consumer sees it) and per node (compute time from perf work records); end-to-end latency is reconstructed frame-by-frame by joining work records along the chain. Y axes are in µs; each chart toggles log/linear independently.</div>

<h2>1. Communication delay per edge (publish → consume)</h2>
<div class="toggle">Y axis: <label><input type='radio' name='ys_edges' value='log' checked> log</label><label><input type='radio' name='ys_edges' value='value'> linear</label></div>
<div class="chart" id="c_edges"></div>
__EDGE_TABLE__

<h2>2. Compute time per node (work delay)</h2>
<div class="toggle">Y axis: <label><input type='radio' name='ys_work' value='log' checked> log</label><label><input type='radio' name='ys_work' value='value'> linear</label></div>
<div class="chart" id="c_work"></div>
__NODE_TABLE__

<h2>3. End-to-end latency (frame-joined)</h2>
<div class="toggle">Y axis: <label><input type='radio' name='ys_e2e' value='log' checked> log</label><label><input type='radio' name='ys_e2e' value='value'> linear</label></div>
<div class="chart" id="c_e2e"></div>
__E2E_TABLE__

<h2>4. Resource usage (CPU % &amp; RSS, 200ms sampling)</h2>
<div class="chart" id="c_cpu"></div>
<div class="chart" id="c_rss"></div>
__RES_TABLE__

<h2>Overall conclusion</h2>
<div class="final">__CONCLUSION__</div>

<p style="color:#6b7280;font-size:12px">data: eval_chains JSON · render: render_eval.py + ECharts inlined, works offline</p>

<script>__ECHARTS__</script>
<script>
const D = __DATA__;
const charts = {};
const ZOOM = [{type: 'inside', xAxisIndex: 0},
              {type: 'slider', xAxisIndex: 0, height: 18, bottom: 2}];
const PALETTE = ['#2563eb','#7c3aed','#0ea5e9','#d946ef','#16a34a','#e11d48',
                 '#ea580c','#0891b2','#9333ea','#ca8a04','#4f46e5','#64748b'];

function mk(id, opt) {
  if (charts[id]) charts[id].dispose();
  const c = echarts.init(document.getElementById(id));
  charts[id] = c;
  c.setOption(opt);
}

function colorize(keys) {
  const m = {};
  keys.forEach((k, i) => { m[k] = PALETTE[i % PALETTE.length]; });
  return m;
}

function lineSeries(src, keys, colorMap, transform) {
  return keys.map(k => ({
    name: k, type: 'line', showSymbol: false, smooth: true,
    lineStyle: {width: 1.2, color: colorMap[k]},
    itemStyle: {color: colorMap[k]},
    data: transform(src[k] || []),
  }));
}

// Samples recorded as 0 sit below the 8.192µs quantization floor. Keep raw
// 0 in the series data and re-derive display points per axis: true 0 on the
// linear axis, clamped to 1µs on the log axis (whose domain starts at 1).
function redata(s, isLog) {
  return s.data.map(p => [p[0], isLog && p[1] === 0 ? 1 : p[1]]);
}
function wireToggle(name, chartId, series) {
  document.querySelectorAll(`input[name=${name}]`).forEach(r =>
    r.addEventListener('change', () => {
      const isLog = r.value === 'log';
      charts[chartId].setOption({
        yAxis: {type: isLog ? 'log' : 'value', min: isLog ? 1 : 0},
        series: series.map(s => ({data: redata(s, isLog)})),
      });
    }));
}

// 1. Edges (per-channel latency series)
(function(){
  const src = D.edge_series || {};
  // Default-visible: up to 12 highest-count edges; legend scroll shows the rest.
  const keys = Object.keys(src);
  const top = keys.slice(0, 12);
  const cmap = colorize(keys);
  const series = lineSeries(src, keys, cmap, pts =>
    pts.map(p => [p[0] / 1000, p[1]]));
  const DEF = Object.fromEntries(keys.map(k => [k, top.indexOf(k) >= 0]));
  mk('c_edges', {
    title: {text: 'per-edge communication latency (publish → consume), µs', textStyle: {fontSize: 13}},
    tooltip: {trigger: 'axis'},
    legend: {bottom: 26, type: 'scroll', selected: DEF},
    dataZoom: ZOOM,
    xAxis: {type: 'value', name: 't (s)'},
    yAxis: {type: 'log', name: 'µs', min: 1},
    grid: {left: 70, right: 30, top: 40, bottom: 84},
    series: series.map(s => ({...s, data: redata(s, true)})),
  });
  wireToggle('ys_edges', 'c_edges', series);
})();

// 2. Work (per-node compute series, productive iterations only)
(function(){
  const src = D.work_series || {};
  const keys = Object.keys(src);
  const cmap = colorize(keys);
  // work records: [t_ms, dur_us, io, flags]; productive = flags & 6
  const series = lineSeries(src, keys, cmap, pts =>
    pts.filter(p => (p[3] & 6) !== 0).map(p => [p[0] / 1000, p[1]]));
  mk('c_work', {
    title: {text: 'compute time per node (productive iterations), µs', textStyle: {fontSize: 13}},
    tooltip: {trigger: 'axis'},
    legend: {bottom: 26, type: 'scroll'},
    dataZoom: ZOOM,
    xAxis: {type: 'value', name: 't (s)'},
    yAxis: {type: 'log', name: 'µs', min: 1},
    grid: {left: 70, right: 30, top: 40, bottom: 84},
    series: series.map(s => ({...s, data: redata(s, true)})),
  });
  wireToggle('ys_work', 'c_work', series);
})();

// 3. E2E per-frame series
(function(){
  const series = [{name: 'e2e', type: 'line', showSymbol: false, smooth: true,
                   lineStyle: {width: 1.4, color: '#2563eb'},
                   itemStyle: {color: '#2563eb'},
                   data: (D.e2e_series || []).map(p => [p[0] / 1000, p[1]])}];
  mk('c_e2e', {
    title: {text: 'end-to-end latency per frame (frame-joined), µs', textStyle: {fontSize: 13}},
    tooltip: {trigger: 'axis'},
    legend: {bottom: 26},
    dataZoom: ZOOM,
    xAxis: {type: 'value', name: 't (s)'},
    yAxis: {type: 'log', name: 'µs', min: 1},
    grid: {left: 70, right: 30, top: 40, bottom: 84},
    series: series.map(s => ({...s, data: redata(s, true)})),
  });
  wireToggle('ys_e2e', 'c_e2e', series);
})();

// 4. Resources (CPU% / RSS per role)
(function(){
  const roles = Object.keys(D.resources || {});
  const cpuSrc = {}, rssSrc = {};
  for (const r of roles) {
    const pts = D.resources[r] || [];
    let prevT = null, prevJ = null;
    cpuSrc[r] = []; rssSrc[r] = [];
    for (const p of pts) {
      const [t, j, rss] = p;
      if (prevT !== null && t > prevT) {
        cpuSrc[r].push([t / 1000, Math.round((j - prevJ) * 1000 / (t - prevT) * 100) / 100]);
      }
      prevT = t; prevJ = j;
      rssSrc[r].push([t / 1000, Math.round(rss / 1024 * 100) / 100]);
    }
  }
  const cmap = colorize(roles);
  function rseries(src) {
    return roles.map(r => ({name: r, type: 'line', showSymbol: false, smooth: true,
      lineStyle: {width: 1.2, color: cmap[r]}, itemStyle: {color: cmap[r]}, data: src[r]}));
  }
  mk('c_cpu', {title: {text: 'role CPU %', textStyle: {fontSize: 13}},
    tooltip: {trigger: 'axis'}, legend: {bottom: 0}, dataZoom: ZOOM,
    xAxis: {type: 'value', name: 't (s)'}, yAxis: {type: 'value', name: '%'},
    grid: {left: 70, right: 30, top: 40, bottom: 84}, series: rseries(cpuSrc)});
  mk('c_rss', {title: {text: 'role RSS MB', textStyle: {fontSize: 13}},
    tooltip: {trigger: 'axis'}, legend: {bottom: 0}, dataZoom: ZOOM,
    xAxis: {type: 'value', name: 't (s)'}, yAxis: {type: 'value', name: 'MB'},
    grid: {left: 70, right: 30, top: 40, bottom: 84}, series: rseries(rssSrc)});
})();

window.addEventListener('resize', () => {
  for (const id in charts) charts[id].resize();
});
</script>
</body></html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="Render an eval_chains JSON to HTML")
    parser.add_argument("--in", dest="in_path", required=True, help="eval_chains JSON")
    parser.add_argument("--out", dest="out_path", default="chain_eval_report.html",
                        help="output HTML path")
    args = parser.parse_args()

    in_path = Path(args.in_path)
    if not in_path.is_file():
        print(f"render_eval: no such file: {in_path}", file=sys.stderr)
        return 2

    echarts_path = Path(__file__).resolve().parent / "echarts.min.js"
    if not echarts_path.is_file():
        print(f"render_eval: missing {echarts_path}", file=sys.stderr)
        return 2

    try:
        doc = _load(in_path)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"render_eval: cannot parse {in_path}: {exc}", file=sys.stderr)
        return 2

    echarts_js = echarts_path.read_text(encoding="utf-8")
    html = render(doc, echarts_js)

    out_path = Path(args.out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(html, encoding="utf-8")
    print(f"rendered: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
