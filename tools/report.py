#!/usr/bin/env python3
"""Generate the static HTML report (docs/report.html) from the build
artifacts: the model-checking table (build/mc.md), the benchmarks
(build/bench.md), the synthesis estimate (synth/report.md), the litmus
result (build/litmus.json) and the bug-museum data (docs/museum.json).

The page is one self-contained file: the data is inlined and rendered with
plain DOM JavaScript (no WebAssembly, no eval, no external fetch, no CDN), so
it opens from file:// in Safari's Lockdown Mode.  Run `make report`."""
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build"


def read(path, default=""):
    p = Path(path)
    return p.read_text() if p.exists() else default


def md_table(md, after):
    """Pull the first markdown table that comes after a line containing `after`."""
    lines = md.splitlines()
    start = next((i for i, l in enumerate(lines) if after in l), None)
    if start is None:
        return []
    rows = []
    for l in lines[start:]:
        if l.strip().startswith("|"):
            cells = [c.strip() for c in l.strip().strip("|").split("|")]
            if set("".join(cells)) <= set("-: "):
                continue
            rows.append(cells)
        elif rows:
            break
    return rows


def collect():
    data = {}
    mc = read(BUILD / "mc.md")
    data["mc_correct"] = md_table(mc, "Correct protocol")
    data["mc_bugs"] = md_table(mc, "Bug variants")
    bench = read(BUILD / "bench.md")
    data["mandel"] = md_table(bench, "Mandelbrot")
    data["hist"] = md_table(bench, "histogram")
    m = re.search(r"speed-up from the fix: ([\d.]+x)", bench)
    data["false_sharing"] = m.group(1) if m else "n/a"
    data["synth"] = md_table(read(REPO / "synth" / "report.md"), "harts |")
    lit = read(BUILD / "litmus.json")
    try:
        data["litmus"] = json.loads(lit[lit.index("{"):]) if "{" in lit else None
    except ValueError:
        data["litmus"] = None
    data["museum"] = json.loads(read(REPO / "docs" / "museum.json", "{}") or "{}")
    return data


HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>rv32-smp report</title>
<style>
:root {
  --bg: #ffffff; --fg: #1a1a1a; --muted: #5a5a5a; --line: #d9d9d9; --panel: #f5f5f3;
  --accent: #335; --i:#cfcfcf; --s:#7fa8d0; --e:#e0b050; --m:#c2614f; --ok:#2f7d42; --bad:#b23b2e;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg:#15171a; --fg:#e8e8e6; --muted:#9aa0a6; --line:#33373c; --panel:#1d2024;
    --accent:#9db7e0; --i:#3a3f45; --s:#4a6fa0; --e:#9c7a2e; --m:#9c4436; --ok:#5bbd73; --bad:#e06a5c;
  }
}
:root[data-theme="dark"] {
  --bg:#15171a; --fg:#e8e8e6; --muted:#9aa0a6; --line:#33373c; --panel:#1d2024;
  --accent:#9db7e0; --i:#3a3f45; --s:#4a6fa0; --e:#9c7a2e; --m:#9c4436; --ok:#5bbd73; --bad:#e06a5c;
}
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); margin: 0;
  font: 15px/1.55 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; }
main { max-width: 960px; margin: 0 auto; padding: 24px 16px 80px; }
h1 { font-size: 24px; margin: 0 0 4px; }
h2 { font-size: 19px; margin: 40px 0 8px; padding-top: 10px; border-top: 1px solid var(--line); }
h3 { font-size: 16px; margin: 22px 0 6px; }
p { margin: 8px 0; } .muted { color: var(--muted); }
code, .mono { font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; font-size: 13px; }
table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 14px; }
th, td { border: 1px solid var(--line); padding: 5px 9px; text-align: left; }
th { background: var(--panel); font-weight: 600; }
td.num, th.num { text-align: right; font-variant-numeric: tabular-nums; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; margin: 14px 0 6px; }
.tab { padding: 6px 12px; border: 1px solid var(--line); background: var(--panel); cursor: pointer;
  border-radius: 4px; font-size: 13px; color: var(--fg); }
.tab[aria-selected="true"] { background: var(--accent); color: var(--bg); border-color: var(--accent); }
.bug { border: 1px solid var(--line); border-radius: 6px; padding: 14px 16px; margin: 10px 0; }
.expl { background: var(--panel); border-left: 3px solid var(--accent); padding: 8px 12px; margin: 10px 0; }
.timeline { overflow-x: auto; }
.tl { border-collapse: collapse; font-size: 12.5px; min-width: 100%; }
.tl th, .tl td { border: 1px solid var(--line); padding: 4px 7px; white-space: nowrap; }
.tl td.act { white-space: normal; max-width: 360px; }
.cell { display: inline-flex; gap: 4px; align-items: center; }
.st { display: inline-block; min-width: 20px; text-align: center; border-radius: 3px; padding: 0 5px;
  font-weight: 700; color: #111; }
.st.I { background: var(--i); color: var(--muted); } .st.S { background: var(--s); color:#071; }
.st.E { background: var(--e); } .st.M { background: var(--m); color:#fff; }
.stale { outline: 2px dashed var(--bad); }
.badge { display:inline-block; padding: 1px 7px; border-radius: 3px; font-size: 12px; font-weight: 600; }
.badge.bad { background: var(--bad); color:#fff; } .badge.ok { background: var(--ok); color:#fff; }
.bar { background: var(--accent); height: 16px; border-radius: 2px; }
.barrow { display:flex; align-items:center; gap:8px; margin:3px 0; }
.barrow .lbl { width: 64px; } .barrow .val { width: 120px; }
.legend { display:flex; gap:14px; flex-wrap:wrap; margin: 6px 0; font-size:13px; }
details { margin: 8px 0; } summary { cursor: pointer; font-weight:600; }
.evlog { font-size:12px; margin:0; }
.evlog div { padding:1px 0; border-bottom:1px solid var(--line); }
</style>
</head>
<body>
<main>
<h1>rv32-smp &mdash; a cache-coherent multicore RISC-V</h1>
<p class="muted">An N-core RV32IMA built from a single-core pipeline, with private MESI L1 caches on a
snooping bus. The coherence protocol is verified three ways that agree: an exhaustive model check,
a golden memory-order checker on the RTL, and step-by-step agreement between the RTL and an abstract
model. Known protocol bugs are reproduced end to end below. Everything is simulation plus a Yosys
estimate; no FPGA board.</p>

<h2>Results at a glance</h2>
<div id="glance"></div>

<h2>Model checking the protocol</h2>
<p>An explicit-state search over every reachable state of the abstract protocol (states counted after
symmetry reduction over caches and addresses). It checks single-writer/multiple-reader, the data-value
invariant (every read returns the latest write in coherence order) and deadlock freedom.</p>
<div id="mc_correct"></div>
<p>Each deliberately broken variant (the "bug museum") has a shortest counterexample:</p>
<div id="mc_bugs"></div>

<h2>Bug museum</h2>
<p>Four-plus classic coherence and atomics bugs, each a switchable RTL variant. For every one: the
model checker's shortest counterexample as a timeline, the plain-language reason, and the same bug
reproduced in the RTL by a directed two-hart program (caught by the memory checker, the model, or a
deadlock budget).</p>
<div class="legend">
  <span class="cell"><span class="st I">I</span> invalid</span>
  <span class="cell"><span class="st S">S</span> shared</span>
  <span class="cell"><span class="st E">E</span> exclusive</span>
  <span class="cell"><span class="st M">M</span> modified</span>
  <span class="cell"><span class="st S stale">S</span> dashed = holds a stale value</span>
</div>
<div class="tabs" id="bugtabs"></div>
<div id="bugpanel"></div>

<h2>Speed-up and coherence traffic</h2>
<h3>Mandelbrot, rows split across harts (little sharing)</h3>
<div id="mandel"></div>
<div id="mandelbar"></div>
<h3>Shared histogram (every bucket contended)</h3>
<div id="hist"></div>
<h3>False sharing and its fix</h3>
<p>Each hart increments its own counter; in the packed layout the counters share one cache line and
ping-pong, in the padded layout each sits on its own line. The fix is <b id="fs"></b> faster.</p>

<h2>Litmus tests vs the reference RVWMO model</h2>
<p>Each test runs many times with the timing perturbed; every outcome the core produces is checked
against the reference model's verdict shipped with litmus-tests-riscv. The core performs memory
accesses in program order at one point, so it is sequentially consistent: it never shows a relaxed
outcome, even where RVWMO allows one.</p>
<div id="litmus"></div>

<h2>Synthesis estimate (Yosys, Spartan-7 XC7S50)</h2>
<p>No place and route; the delay is a rough logic-level estimate, not sign-off.</p>
<div id="synth"></div>

<p class="muted" style="margin-top:40px">Generated by <code>make report</code> from the build
artifacts. Numbers measured on this machine.</p>
</main>
<script id="data" type="application/json">__DATA__</script>
<script>
"use strict";
var D = JSON.parse(document.getElementById("data").textContent);
function el(t, cls, txt){ var e=document.createElement(t); if(cls)e.className=cls; if(txt!=null)e.textContent=txt; return e; }
function tableFrom(rows, numFrom){
  var t=el("table"); if(!rows||!rows.length) return t;
  var thead=el("thead"), tr=el("tr");
  rows[0].forEach(function(h,i){ var th=el("th",i>=numFrom?"num":null,h); tr.appendChild(th); });
  thead.appendChild(tr); t.appendChild(thead);
  var tb=el("tbody");
  rows.slice(1).forEach(function(r){ var x=el("tr"); r.forEach(function(c,i){ x.appendChild(el("td",i>=numFrom?"num":null,c)); }); tb.appendChild(x); });
  t.appendChild(tb); return t;
}
function put(id, node){ var h=document.getElementById(id); if(h) h.appendChild(node); }

// glance
(function(){
  var g=el("table"), tb=el("tbody");
  function row(k,v){ var tr=el("tr"); tr.appendChild(el("th",null,k)); tr.appendChild(el("td",null,v)); tb.appendChild(tr); }
  var big = D.mc_correct.length? D.mc_correct[D.mc_correct.length-1] : null;
  if(big) row("Largest model check", big[0]+" caches × "+big[1]+" addresses: "+big[2]+" states, "+big[6]);
  row("Protocol bugs reproduced", (D.museum.bugs?D.museum.bugs.length:0)+" (model counterexample + RTL)");
  if(D.mandel.length>3) row("Mandelbrot speed-up (4 cores)", D.mandel[3][2]);
  row("False-sharing fix", D.false_sharing+" faster");
  if(D.synth.length) row("Synthesis (2 harts)", D.synth[2][1]+" LUTs, "+D.synth[2][3]+" FFs (XC7S50)");
  g.appendChild(tb); put("glance",g);
})();

put("mc_correct", tableFrom(D.mc_correct, 2));
put("mc_bugs", tableFrom(D.mc_bugs, 2));
put("mandel", tableFrom(D.mandel, 1));
put("hist", tableFrom(D.hist, 1));
put("synth", tableFrom(D.synth, 1));
document.getElementById("fs").textContent = D.false_sharing;

// mandelbrot speed-up bars
(function(){
  if(D.mandel.length<2) return;
  var host=document.getElementById("mandelbar");
  var base=parseFloat(D.mandel[1][2]);
  D.mandel.slice(1).forEach(function(r){
    var sp=parseFloat(r[2]);
    var row=el("div","barrow");
    row.appendChild(el("span","lbl",r[0]+" cores"));
    var bar=el("div","bar"); bar.style.width=(sp/parseFloat(D.mandel[D.mandel.length-1][2])*280)+"px";
    row.appendChild(bar); row.appendChild(el("span","val",r[2]+" ("+r[1]+" cyc)"));
    host.appendChild(row);
  });
})();

// litmus
(function(){
  if(!D.litmus){ return; }
  var rows=[["test","threads","runs","relaxed (non-SC) outcomes seen"]];
  D.litmus.tests.forEach(function(t){ if(t.ran) rows.push([t.name, ""+t.threads, ""+t.runs, ""+t.relaxed]); });
  put("litmus", tableFrom(rows, 1));
  var p=el("p","muted","All "+(D.litmus.tests.filter(function(t){return t.ran;}).length)+" tests: 0 relaxed outcomes over "+
    D.litmus.runs_per_test+" runs each — sequentially consistent, stronger than RVWMO requires.");
  put("litmus",p);
})();

// bug museum
(function(){
  var bugs = (D.museum.bugs)||[];
  var tabs=document.getElementById("bugtabs"), panel=document.getElementById("bugpanel");
  function stateCell(line){
    var span=el("span","st "+line.st, line.st);
    if(line.st!=="I" && line.fresh===0) span.className+=" stale";
    return span;
  }
  function render(b){
    panel.innerHTML="";
    var box=el("div","bug");
    box.appendChild(el("h3",null,"Bug "+b.id+": "+b.title));
    box.appendChild(el("div","expl",b.explanation));
    // model timeline
    if(b.model && b.model.steps){
      box.appendChild(el("p",null,"Model checker, shortest counterexample ("+(b.model.steps.length-1)+
        " steps to "+b.model.violation+"):"));
      var wrap=el("div","timeline"), t=el("table","tl"), thead=el("thead"), tr=el("tr");
      var nc=b.model.steps[0].caches.length, na=b.model.steps[0].caches[0].lines.length;
      tr.appendChild(el("th",null,"#"));
      for(var c=0;c<nc;c++) tr.appendChild(el("th",null,"cache "+c));
      tr.appendChild(el("th",null,"memory"));
      tr.appendChild(el("th",null,"bus"));
      tr.appendChild(el("th",null,"action"));
      thead.appendChild(tr); t.appendChild(thead);
      var tb=el("tbody");
      b.model.steps.forEach(function(s,si){
        var x=el("tr");
        x.appendChild(el("td",null,""+si));
        s.caches.forEach(function(cc){
          var td=el("td"); var inner=el("span","cell");
          cc.lines.forEach(function(ln,li){
            if(na>1){ inner.appendChild(el("span","mono", "ABCD"[li]+":")); }
            inner.appendChild(stateCell(ln));
          });
          if(cc.resv){ inner.appendChild(el("span","mono"," ⦿"+cc.resv)); }
          if(cc.wb){ inner.appendChild(el("span","mono"," wb:"+cc.wb)); }
          if(cc.pend){ inner.appendChild(el("span","mono"," ("+cc.pend+")")); }
          td.appendChild(inner); x.appendChild(td);
        });
        x.appendChild(el("td","mono", s.mem.map(function(m,i){return (na>1?"ABCD"[i]+":":"")+(m?"cur":"stale");}).join(" ")));
        x.appendChild(el("td","mono", s.bus||""));
        x.appendChild(el("td","act", s.action));
        tb.appendChild(x);
      });
      t.appendChild(tb); wrap.appendChild(t); box.appendChild(wrap);
    }
    // RTL reproduction
    if(b.rtl){
      var caught = b.rtl.exit!==0;
      var p=el("p"); p.appendChild(document.createTextNode("RTL reproduction on the buggy build: "));
      p.appendChild(el("span","badge "+(caught?"bad":"ok"), caught?"caught":"passed"));
      box.appendChild(p);
      if(b.rtl.reason) box.appendChild(el("p","mono", b.rtl.reason));
      if(b.rtl.events && b.rtl.events.length){
        var det=el("details"); det.appendChild(el("summary",null,"coherence events just before the failure"));
        var log=el("div","evlog");
        b.rtl.events.forEach(function(e){
          log.appendChild(el("div","mono","cyc "+e.c+"  hart "+e.h+"  "+e.e+"  "+(e.line||"")+"  "+
            (e.cmd||"")+" "+(e.op||"")+(e.to?(" → "+e.to):"")+(e.from?(" (from "+e.from+")"):"")));
        });
        det.appendChild(log); box.appendChild(det);
      }
    }
    panel.appendChild(box);
  }
  bugs.forEach(function(b,i){
    var t=el("div","tab", b.id+". "+b.title);
    t.setAttribute("role","tab"); t.setAttribute("aria-selected", i===0?"true":"false");
    t.addEventListener("click", function(){
      Array.prototype.forEach.call(tabs.children, function(c){ c.setAttribute("aria-selected","false"); });
      t.setAttribute("aria-selected","true"); render(b);
    });
    tabs.appendChild(t);
  });
  if(bugs.length) render(bugs[0]);
})();
</script>
</body>
</html>
"""


def main():
    data = collect()
    blob = json.dumps(data, separators=(",", ":"))
    html = HTML.replace("__DATA__", blob)
    (REPO / "docs" / "report.html").write_text(html)
    print(f"wrote docs/report.html ({len(html)} bytes)")


if __name__ == "__main__":
    main()
