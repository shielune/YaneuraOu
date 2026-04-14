// Turn a path (or URL) to a built `yaneuraou.<pkg>.js` into a LoaderContext,
// and pick the correct per-generation loader for the environment.
//
// Pure string manipulation so this file works unchanged in Node and in the
// browser. In the browser we typically can't detect the emscripten version
// from the served URL (`/engine/yaneuraou.k-p.js`), so the caller is
// expected to pass `versionOverride` from an out-of-band source.

import type { Loader, LoaderContext } from "./types.ts";
import { classifyVersion, type LoaderGeneration } from "./version.ts";

function splitPath(p: string): string[] {
  return p.split(/[/\\]/).filter((s) => s.length > 0);
}

function dirname(p: string): string {
  const i = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"));
  return i >= 0 ? p.slice(0, i) : "";
}

/**
 * Expected layout:
 *   build/<version>_<arch>/<pkg>/<variant>/lib/yaneuraou.<pkg>.js
 *
 * Where `<variant>` is `web` or `node` (produced as two separate builds
 * from the same source by script/wasm_build.js). We walk up to
 * `<version>_<arch>` and strip the arch suffix. Caller should pass
 * `versionOverride` if the path doesn't follow the convention (e.g. an
 * install-dir copy or a browser URL).
 *
 * The old layout without the `<variant>` level is also tolerated for
 * any build artefacts produced before the split landed.
 */
export function detectVersionFromJsPath(jsPath: string): string {
  const parts = splitPath(jsPath);
  if (parts.length < 4) {
    throw new Error(
      `could not extract emscripten version from path '${jsPath}'. ` +
        `Path is too shallow for build/<version>_<arch>/<pkg>/<variant>/lib/<file>.js`,
    );
  }
  // Try new layout first (depth 5: <ver>/<pkg>/<variant>/lib/<file>)
  let verFolder = parts.length >= 5 ? parts[parts.length - 5]! : "";
  // `<version>_<arch>` where <arch> may itself contain underscores
  // (e.g. "3.1.43_x86_64"). Strip from the first underscore onward.
  let ver = verFolder.replace(/_.*$/, "");
  if (!/^\d+\.\d+\.\d+$/.test(ver)) {
    // Fall back to old layout (depth 4: <ver>/<pkg>/lib/<file>)
    verFolder = parts[parts.length - 4]!;
    ver = verFolder.replace(/_.*$/, "");
  }
  if (!/^\d+\.\d+\.\d+$/.test(ver)) {
    throw new Error(
      `could not extract emscripten version from path '${jsPath}' ` +
        `(extracted '${ver}'). Expected ` +
        `build/<version>_<arch>/<pkg>/<variant>/lib/<file>.js`,
    );
  }
  return ver;
}

/**
 * Detect the build variant (web vs node) from a path following the new
 * layout. Returns null if the path is the old layout without the variant
 * level.
 */
export function detectVariantFromJsPath(jsPath: string): "web" | "node" | null {
  const parts = splitPath(jsPath);
  // New layout has <variant> at parts[length - 3].
  if (parts.length < 5) return null;
  const candidate = parts[parts.length - 3];
  if (candidate === "web" || candidate === "node") return candidate;
  return null;
}

export function buildContext(
  environment: "node" | "browser",
  jsPath: string,
  versionOverride?: string,
): LoaderContext {
  const libDir = dirname(jsPath);
  const wasmPath = jsPath.replace(/\.js$/, ".wasm");
  const emscriptenVersion =
    versionOverride ?? detectVersionFromJsPath(jsPath);
  return { environment, libDir, jsPath, wasmPath, emscriptenVersion };
}

/**
 * Pick the first registered loader that claims the version. Registry order
 * matters — more specific matches should be registered first.
 */
export function pickLoader(
  loaders: readonly Loader[],
  version: string,
): Loader {
  const generation = classifyVersion(version);
  const l = loaders.find((ld) => ld.matches(version));
  if (!l) {
    throw new Error(
      `no loader registered for emscripten ${version} (generation=${generation})`,
    );
  }
  return l;
}

export { classifyVersion };
export type { LoaderGeneration };
