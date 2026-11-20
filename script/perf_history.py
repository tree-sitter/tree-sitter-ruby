#!/usr/bin/env python3
"""Writes an HTML page with the parse cost and the parser size of every release since v0.23.1, and of master when
its src/ differs from the last release.

Each point builds script/sanitize.c against that commit's src/ and counts its instructions with valgrind over the Ruby
files under DIR..., like script/perf_guard.sh. Results are kept in CACHE_DIR per src/ tree and corpus, so a run only
measures what is new. usage: script/perf_history.py OUT.html CACHE_DIR DIR...   (playground.yml publishes perf.html)
"""
import concurrent.futures
import glob
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

out, cache, dirs = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
cache.mkdir(parents=True, exist_ok=True)
lock = Path("Cargo.lock").read_text()
version = re.search(r'name = "tree-sitter"\nversion = "([^"]+)"', lock).group(1)
lib = glob.glob(os.path.expanduser(f"~/.cargo/registry/src/*/tree-sitter-{version}"))[0]
files = sorted(str(p) for d in dirs for p in Path(d).rglob("*.rb"))
digest = hashlib.sha1()
for f in files:
    digest.update(f.encode() + b"\0" + Path(f).read_bytes())
corpus = digest.hexdigest()[:12]


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout.strip()


def semver(tag):
    return tuple(int(n) for n in tag[1:].split("."))


tags = [t for t in git("tag", "-l", "v*").split() if re.fullmatch(r"v\d+\.\d+\.\d+", t) and semver(t) >= (0, 23, 1)]
# A point is a version of src/, so commits that leave src/ alone are not measured again
points = [(t, git("rev-parse", f"{t}:src")) for t in sorted(tags, key=semver)]
if git("rev-parse", "HEAD:src") != points[-1][1]:
    points.append(("master", git("rev-parse", "HEAD:src")))


def measure(tree):
    saved = cache / f"{tree}-{corpus}.json"
    if saved.exists():
        return json.loads(saved.read_text())
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp, "src")
        src.mkdir()
        archive = subprocess.run(["git", "archive", tree], check=True, capture_output=True).stdout
        subprocess.run(["tar", "-x", "-C", src], input=archive, check=True)
        parser = (src / "parser.c").read_text()
        # lib/src has the runtime's own ICU headers, so no system ICU is needed
        subprocess.run(["cc", "-O2", f"-I{src}", f"-I{lib}/include", f"-I{lib}/src", "script/sanitize.c", f"{src}/parser.c",
                        f"{src}/scanner.c", f"{lib}/src/lib.c", "-o", f"{tmp}/parse"], check=True)
        subprocess.run(["valgrind", "--tool=cachegrind", "--cache-sim=no", "--cachegrind-out-file=/dev/null",
                        f"--log-file={tmp}/log", f"{tmp}/parse", *files], check=True, stdout=subprocess.DEVNULL)
        refs = re.search(r"I\s+refs:\s+([\d,]+)", Path(tmp, "log").read_text()).group(1)
    result = {"instructions": int(refs.replace(",", "")), "bytes": len(parser.encode()),
              "states": int(re.search(r"#define STATE_COUNT (\d+)", parser).group(1))}
    saved.write_text(json.dumps(result))
    return result


with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
    results = list(pool.map(measure, (tree for _, tree in points)))
rows = [{"label": label, **r} for (label, _), r in zip(points, results)]


def bars(title, key, scale, unit):
    """A bar per point, with its value on top."""
    width, height, top, bottom = 640, 220, 28, 40
    peak = max(r[key] for r in rows) / scale
    step = width / len(rows)
    parts = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title)}">']
    for i, r in enumerate(rows):
        value = r[key] / scale
        h = (height - top - bottom) * value / peak
        x, y = i * step + step * 0.2, height - bottom - h
        parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{step * 0.6:.1f}" height="{h:.1f}" rx="3"/>')
        parts.append(f'<text x="{x + step * 0.3:.1f}" y="{y - 6:.1f}" class="value">{value:.2f}{unit}</text>')
        parts.append(f'<text x="{x + step * 0.3:.1f}" y="{height - bottom + 18}">{html.escape(r["label"])}</text>')
    parts.append("</svg>")
    return f"<h2>{html.escape(title)}</h2>" + "".join(parts)


table = "".join(
    f"<tr><td>{html.escape(r['label'])}</td>"
    f"<td>{r['instructions']:,}</td><td>{r['bytes'] / 1e6:.1f} MB</td><td>{r['states']:,}</td></tr>"
    for r in rows)
out.write_text(f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>tree-sitter-ruby performance</title>
<style>
  :root {{ --bg: #ffffff; --fg: #1f2328; --muted: #59636e; --bar: #0969da; --line: #d1d9e0; }}
  @media (prefers-color-scheme: dark) {{
    :root {{ --bg: #0d1117; --fg: #e6edf3; --muted: #9198a1; --bar: #4493f8; --line: #3d444d; }}
  }}
  body {{ background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, sans-serif; margin: 0 auto;
         max-width: 720px; padding: 24px 16px; }}
  h1 {{ font-size: 1.4rem; }} h2 {{ font-size: 1.05rem; margin-top: 2rem; }}
  p {{ color: var(--muted); }} a {{ color: var(--bar); }}
  svg {{ width: 100%; height: auto; }} rect {{ fill: var(--bar); }}
  text {{ fill: var(--muted); font-size: 12px; text-anchor: middle; }} text.value {{ fill: var(--fg); }}
  table {{ border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }}
  th, td {{ border-bottom: 1px solid var(--line); padding: 6px 8px; text-align: right; }}
  th:first-child, td:first-child {{ text-align: left; }}
  .scroll {{ overflow-x: auto; }}
</style>
</head>
<body>
<h1>tree-sitter-ruby performance</h1>
<p>Instructions counted by valgrind to parse the same {len(files):,} Ruby files (ruby/spec and the highlight tests) with
each release's parser, and the size of its generated <code>parser.c</code>. Lower is better.
<a href="https://github.com/Magikdidi24/tree-sitter-ruby">Repository</a> ·
<a href="./">Playground</a></p>
{bars("Instructions to parse (billions)", "instructions", 1e9, "")}
{bars("parser.c (MB)", "bytes", 1e6, "")}
<h2>All numbers</h2>
<div class="scroll"><table>
<tr><th>Version</th><th>Instructions</th><th>parser.c</th><th>States</th></tr>
{table}
</table></div>
</body>
</html>
""")
print(json.dumps(rows, indent=1))
