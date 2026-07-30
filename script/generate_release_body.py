#!/usr/bin/env python3
"""Generate the GitHub Release body for the WASM packages.

Reads the matrix from .github/workflows/build-wasm.yml (the Single Source of
Truth for what packages exist, what their categories are, what eval data
they need, etc.) and emits a Markdown body identical in shape to what we
previously wrote inline in the workflow file.

The point of this script is that adding/removing a WASM package becomes a
matrix-only edit — the for-loop in release-wasm, the files: glob, the
Markdown tables in the body, and the literal counts ("Ten", "Four")
all derive from the matrix automatically.

The per-release prose ("what's new in this one") comes from
docs/releases/<version>.md instead, so release notes are reviewable in the
repository and can be regenerated to fix a published release. See
docs/releases/README.md.

When the Node loader API changes, or the eval/book URL tables need updating,
edit the static prose in the middle of this file (search for `STATIC_*`).

Usage:
    python3 script/generate_release_body.py \\
        --workflow .github/workflows/build-wasm.yml \\
        --version  8.50.0 \\
        --output   /tmp/release_body.md
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    print(
        "ERROR: PyYAML is required (`pip install pyyaml`). "
        "On ubuntu-latest GitHub Actions runners it is preinstalled.",
        file=sys.stderr,
    )
    sys.exit(1)


# ---------------------------------------------------------------------------
# Numeric → English (lowercase) up to 30. Above 30 we just print the digits.
# Used for headline literals like "Twenty npm-shaped packages" and
# "Six brand-new" so they auto-update when the matrix grows.
# ---------------------------------------------------------------------------
_WORDS = [
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
    "twenty-one", "twenty-two", "twenty-three", "twenty-four", "twenty-five",
    "twenty-six", "twenty-seven", "twenty-eight", "twenty-nine", "thirty",
]


def word(n: int) -> str:
    return _WORDS[n] if 0 <= n < len(_WORDS) else str(n)


def Word(n: int) -> str:  # capitalised at start of sentence
    w = word(n)
    return w[0].upper() + w[1:]


# ---------------------------------------------------------------------------
# Field-to-string converters
# ---------------------------------------------------------------------------
def fmt_memory(initial: int, maximum: int) -> str:
    """134217728 -> "128 MB", 2147483648 -> "2 GB"."""
    def one(b: int) -> str:
        gb = b / (1 << 30)
        if gb >= 1 and gb == int(gb):
            return f"{int(gb)} GB"
        if gb >= 1:
            return f"{gb:.1f} GB"
        mb = b / (1 << 20)
        return f"{int(mb)} MB"
    return f"{one(initial)} / {one(maximum)}"


def fmt_threading(category: str, pthread: int) -> str:
    # node/pthread categories all use pthread; cfworkers categories are
    # single-thread. (Reading `pthread:` directly is equivalent but
    # category is more explicit for the table.)
    return "pthread" if pthread == 1 else "single-thread"


_TARGET_BY_CATEGORY = {
    "cfworkers":      "Cloudflare Workers, edge runtimes",
    "cfworkers-hlsl": "Cloudflare Workers, edge runtimes",
    "pthread":        "Browser (COOP/COEP required)",
    "pthread-hlsl":   "Browser (COOP/COEP required)",
    "node":           "Node.js (worker_threads)",
}


def target_for(category: str) -> str:
    if category not in _TARGET_BY_CATEGORY:
        raise SystemExit(f"unknown category: {category!r} — update _TARGET_BY_CATEGORY")
    return _TARGET_BY_CATEGORY[category]


def npm_name(dir_: str) -> str:
    """dir 'yaneuraou-wasm-node-kp256' -> '@ultemica/yaneuraou-wasm-node-kp256'."""
    return f"@ultemica/{dir_}"


# ---------------------------------------------------------------------------
# Table builders. Each takes a list of matrix entries (already filtered by
# category) and returns a Markdown table as a string.
# ---------------------------------------------------------------------------
def table_default(rows: list[dict]) -> str:
    out = [
        "| Package | Engine | Eval data | Threading | Initial / Max memory | Target |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        out.append(
            f"| `{npm_name(r['dir'])}` "
            f"| {r['engine_label']} "
            f"| {r['eval_note']} "
            f"| {fmt_threading(r['category'], r['pthread'])} "
            f"| {fmt_memory(r['initial_memory'], r['maximum_memory'])} "
            f"| {target_for(r['category'])} |"
        )
    return "\n".join(out)


def table_hlsl(rows: list[dict]) -> str:
    """hlsl table has Package + Notes only — Notes derived from engine + category."""
    out = [
        "| Package | Notes |",
        "|---|---|",
    ]
    for r in rows:
        # Notes string: "<engine short> <variant kind> + humanlike"
        # Map engine_label to a short tag using a small switch.
        engine_short = _engine_short_label(r["engine_label"])
        kind = "cfworkers" if r["category"] == "cfworkers-hlsl" else "pthread"
        out.append(
            f"| `{npm_name(r['dir'])}` | {engine_short} {kind} + humanlike |"
        )
    return "\n".join(out)


def _engine_short_label(engine_label: str) -> str:
    """e.g. 'NNUE HalfKP_256x2_32_32 (Suisho5)' -> 'HalfKP256'."""
    if engine_label.startswith("NNUE KP256"):
        return "KP256"
    if engine_label.startswith("NNUE HalfKP_256x2_32_32"):
        return "HalfKP256"
    if engine_label.startswith("NNUE HalfKP_768x2_16_64"):
        return "HalfKP768"
    if engine_label.startswith("Material"):
        return "Material Lv1"
    if engine_label.startswith("Mobility"):
        return "Mobility"
    if engine_label.startswith("Mate"):
        return "Mate"
    # Fallback: first word
    return engine_label.split()[0]


def table_node(rows: list[dict]) -> str:
    """Node table is like default but without the Target column.

    Sort rows by (initial_memory ASC, original-matrix-index) so the 128 MB
    KP256/Mate/Material/Mobility group renders before the 256 MB HalfKP
    pair. This matches the hand-curated ordering of the previous body.
    """
    sorted_rows = sorted(
        enumerate(rows),
        key=lambda ix_r: (ix_r[1]["initial_memory"], ix_r[0]),
    )
    out = [
        "| Package | Engine | Eval data | Threading | Initial / Max memory |",
        "|---|---|---|---|---|",
    ]
    for _idx, r in sorted_rows:
        out.append(
            f"| `{npm_name(r['dir'])}` "
            f"| {r['engine_label']} "
            f"| {r['eval_note']} "
            f"| {fmt_threading(r['category'], r['pthread'])} "
            f"| {fmt_memory(r['initial_memory'], r['maximum_memory'])} |"
        )
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Static prose. These do not depend on the matrix and need manual editing
# when the loader API changes / a new benchmark is run / etc.
# ---------------------------------------------------------------------------
STATIC_NODE_QUICKSTART = """**Quickstart (Node 18+):**

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-kp256";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-kp256/engine";

const require = createRequire(import.meta.url);
const wasmPath   = require.resolve("@ultemica/yaneuraou-wasm-node-kp256/wasm");
const enginePath = require.resolve("@ultemica/yaneuraou-wasm-node-kp256/engine");
const wasmBinary = await fs.readFile(wasmPath);
const evalBin    = await fs.readFile("./eval/nn.bin"); // suishopetite for KP256

const engine = await createEngine({
  factory: YaneuraOuFactory,
  enginePath,
  wasmBinary,
  evalBin,
  threads: 4,
  usiHash: 64,
});

const result = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 2_000,
});
console.log(result.bestmove, result.score); // -> "7g7f", { kind: "cp", value: ... }
engine.dispose();
```

> Requires **Node.js 18 or later**. Browsers cannot load these binaries (built with `EM_ENVIRONMENT=node`, not `web,worker`); use the `pthread-*` packages there. Cloudflare Workers / V8 Isolate environments can't host pthread builds at all; use the `*-cfworkers` packages there."""

STATIC_HLSL_NOTES = """> Mate engine variants are not provided in -hlsl form because humanlike options are search-time personality filters and have no effect on the DfPn mate solver."""

STATIC_VARIANT_NOTES = """> Cloudflare Workers cannot host pthread builds — use the cfworkers variant there. HalfKP cfworkers variant does not exist (eval exceeds Workers memory budget)."""

STATIC_PERFORMANCE_SECTION = """## Performance — how close to native?

WASM is not the slow option. Measured on one machine with the same eval and the
same search parameters, **WASM reaches about 92% of a native SIMD build**.

| Build | nps | vs native NEON |
|---|---|---|
| native clang-14 (NEON) | 764k | 102% |
| native gcc (NEON) | 748k | 100% |
| **wasm32 pthread** | **691k** | **92%** |
| wasm32 (no SIMD) | 585k | 78% |
| native clang-14 (scalar) | 320k | 43% |
| native gcc (scalar) | 203k | 27% |

`Threads=1` / `go nodes 1000000` / KP256 + suishopetite / median of 3 runs.
Absolute numbers are machine-dependent — only the ratios carry over.

- **WASM SIMD is doing real work.** Turning it off drops 691k to 585k (-15%).
  WASM still beats both native scalar builds by more than 2x.
- **Threads scale.** On Node, 1 to 4 threads goes 686k to 2,916k (4.25x) —
  superlinear thanks to the shared transposition table.
- **Browser and Node are within noise.** 99% at 1 thread, 92% at 4 threads;
  `SharedArrayBuffer` synchronisation costs the browser a little.

Search results are **bit-identical across every WASM variant** (32/64-bit,
SIMD on/off, browser/Node/edge) down to the node count, so switching runtimes
does not change what the engine plays.

Full conditions and raw data: `docs/reports/2026-07-28_wasm_v96x_performance.md`."""

STATIC_EVAL_SECTION = """## Required: NNUE eval file (NNUE builds only)

WASM bundle には評価関数を内蔵していないので、別途 nn.bin を取得して `FS.writeFile` で MEMFS に書いて `EvalDir` USI option で読み込ませる。

| Eval | Engine | Size | URL |
|---|---|---|---|
| suisho5 nn.bin | HalfKP_256x2_32_32 | ~62 MB | https://github.com/mizar/YaneuraOu/releases/download/resource/suisho5_20211123.halfkp.nnue.cpp.gz |
| AobaNNUE nn.bin | HalfKP_768x2_16_64 | ~184 MB | https://github.com/yssaya/AobaNNUE/releases |
| suishopetite nn.bin | KP256 | ~873 KB | https://github.com/mizar/YaneuraOu/releases/download/resource/suishopetite_20211123.k_p.nnue.cpp.gz |

> suisho5 / suishopetite は embedded C++ array 形式の `.cpp.gz`。`script/eval_bin_to_cpp_literal.py` の逆変換で `.bin` に戻す。AobaNNUE は素の `nn.bin` がそのまま配布されている。

> KP256 / HalfKP_256x2_32_32 / HalfKP_768x2_16_64 の eval はそれぞれ互換性なし。Mate engine は eval 不要。HalfKP eval (62 MB) は cfworkers の 128 MB heap で OOM になるため pthread variant でのみ使える。"""

STATIC_BOOK_SECTION = """## Optional: opening book

`BookDir` / `BookFile` USI option で読み込ませる。

| Book | 局面数 | Size | URL |
|---|---|---|---|
| 100T-shock | ~40,000 | 4.7 MB | https://github.com/yaneurao/YaneuraOu/releases/download/BOOK-100T-Shock/100T-shock-book.zip |
| 700T-shock | ~400,000 | 32 MB | https://github.com/yaneurao/YaneuraOu/releases/download/BOOK-700T-Shock/700T-shock-book.zip |
| 新ペタショック 233万 | ~2,330,000 | 76 MB (.7z) | https://github.com/yaneurao/YaneuraOu/releases/download/new_petabook233/new_petabook_20250505c.7z |

> 700T-shock (32 MB) は cfworkers の 128 MB heap だと OOM。cfworkers で book を使う場合は 100T-shock まで。petabook は pthread variant でも heap 圧迫するので注意。"""


# ---------------------------------------------------------------------------
# Top-level assembly
# ---------------------------------------------------------------------------
def load_headline(version: str, notes_dir: Path) -> str | None:
    """Read the per-release prose from docs/releases/<version>.md.

    Returns None when the file does not exist, so a tag can be cut without
    hand-writing notes first.
    """
    path = notes_dir / f"{version}.md"
    if not path.is_file():
        return None
    text = path.read_text().strip()
    return text or None


def build_body(packages: list[dict], headline: str | None = None) -> str:
    by_cat: dict[str, list[dict]] = {}
    for p in packages:
        cat = p.get("category")
        if cat not in _TARGET_BY_CATEGORY:
            raise SystemExit(
                f"matrix entry {p.get('name', '<unknown>')!r} has unknown "
                f"category {cat!r}. Add it to _TARGET_BY_CATEGORY in "
                f"script/generate_release_body.py."
            )
        by_cat.setdefault(cat, []).append(p)

    default_rows = by_cat.get("cfworkers", []) + by_cat.get("pthread", [])
    hlsl_rows = by_cat.get("cfworkers-hlsl", []) + by_cat.get("pthread-hlsl", [])
    node_rows = by_cat.get("node", [])

    total = len(packages)
    n_node = len(node_rows)

    # Per-release prose lives in docs/releases/<version>.md; without it we
    # still emit a usable body describing the package set as the matrix has it.
    if headline is None:
        headline = (
            f"**{Word(total)} npm-shaped packages**, each shipped as its own "
            f"`tar.gz` asset on this page."
        )

    intro = (
        f"YaneuraOu shogi engine — WebAssembly builds.\n"
        f"\n"
        f"**What's new in this release**: {headline}\n"
        f"\n"
        f"---\n"
        f"\n"
        f"Each archive contains the package's `package.json` (version "
        f"bumped to this release), `README.md`, `SPEC.md`, the TypeScript "
        f"loader (`dist/index.js`, `dist/index.d.ts`), and the engine "
        f"bundle (`dist/yaneuraou.js`, `dist/yaneuraou.wasm`, `.wasm.br`). "
        f"Node variants additionally ship `dist/yaneuraou.worker.js` (the "
        f"emscripten 3.1.43 classic pthread worker) and "
        f"`dist/worker_shim.{{js,d.ts}}` (the `worker_threads`-side glue "
        f"the loader uses to bridge `.worker.js` into a fake web-worker "
        f"scope)."
    )

    sections = [intro, "## Packages"]
    if default_rows:
        sections.append("### Browser / Cloudflare Workers")
        sections.append(table_default(default_rows))
    # hlsl variants are built only when the matrix carries them; the section
    # disappears entirely otherwise rather than rendering an empty heading.
    if hlsl_rows:
        sections.append("### HumanLike SkillLevel (hlsl) variants")
        sections.append(
            f"Same {word(len(hlsl_rows))} engine-bearing variants above but "
            f"built with `USE_HUMANLIKE_OPTIONS=ON`, exposing the humanlike "
            f"personality USI options (`ForceCaptureProb`, `StableKingProb`, "
            f"`*BlindProb`, etc.) for skill-level tuning / personality "
            f"emulation."
        )
        sections.append(table_hlsl(hlsl_rows))
        sections.append(STATIC_HLSL_NOTES)
    sections.append(STATIC_VARIANT_NOTES)
    sections.append("### Node.js variants")
    sections.append(
        f"{Word(n_node)} packages built with `EM_ENVIRONMENT=node`, "
        f"`EM_PTHREAD=1`, and `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall',"
        f"'callMain']`. Pinned to **emscripten 3.1.43** — the only "
        f"toolchain currently verified to drive a multi-threaded "
        f"YaneuraOu correctly under Node (3.1.44–3.1.73 stall on ESM "
        f"workers, 3.1.74+ stall on `INCOMING_MODULE_JS_API`; both "
        f"unresolved). See `docs/wasm_client_usage.md` for the full "
        f"compatibility matrix."
    )
    if node_rows:
        sections.append(table_node(node_rows))
    sections.append(STATIC_NODE_QUICKSTART)
    sections.append(STATIC_PERFORMANCE_SECTION)
    sections.append(STATIC_EVAL_SECTION)
    sections.append(STATIC_BOOK_SECTION)

    return "\n\n".join(sections) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--workflow",
        type=Path,
        required=True,
        help="path to .github/workflows/build-wasm.yml",
    )
    ap.add_argument(
        "--version",
        required=True,
        help='release version like "8.50.0". Selects the prose file '
             "docs/releases/<version>.md.",
    )
    ap.add_argument(
        "--notes-dir",
        type=Path,
        default=Path("docs/releases"),
        help="directory holding the per-release prose (default: docs/releases)",
    )
    ap.add_argument(
        "--output",
        type=Path,
        required=True,
        help="path to write the release body Markdown to",
    )
    args = ap.parse_args()

    wf = yaml.safe_load(args.workflow.read_text())
    packages = wf["jobs"]["build-wasm"]["strategy"]["matrix"]["package"]
    headline = load_headline(args.version, args.notes_dir)
    if headline is None:
        print(
            f"NOTE: {args.notes_dir / (args.version + '.md')} not found — "
            f"using the generic package-count headline.",
            file=sys.stderr,
        )
    body = build_body(packages, headline)
    args.output.write_text(body)
    print(f"wrote {args.output} ({len(body)} bytes, {body.count(chr(10))} lines)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
