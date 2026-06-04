// Tiny semver comparison helper. Not a real semver implementation — just
// enough for the emscripten tags we care about (all x.y.z, no prerelease).

export type SemVer = readonly [number, number, number];

export function parseVersion(s: string): SemVer {
  const m = s.match(/^(\d+)\.(\d+)\.(\d+)/);
  if (!m) throw new Error(`invalid version: ${s}`);
  return [Number(m[1]), Number(m[2]), Number(m[3])] as const;
}

export function compareVersions(a: string, b: string): number {
  const pa = parseVersion(a);
  const pb = parseVersion(b);
  for (let i = 0; i < 3; i++) {
    if (pa[i] !== pb[i]) return pa[i] - pb[i];
  }
  return 0;
}

export const gte = (a: string, b: string): boolean => compareVersions(a, b) >= 0;
export const lt = (a: string, b: string): boolean => compareVersions(a, b) < 0;
export const eq = (a: string, b: string): boolean => compareVersions(a, b) === 0;

/**
 * How a given emscripten version shapes the generated JS, bucketed into the
 * three loader strategies we need to support.
 *
 * - `classic-worker`: 3.1.43 — separate `yaneuraou.<pkg>.worker.js` is a
 *   classic script. Main thread drives the engine via `Module.postMessage`.
 * - `esmodule-worker`: 3.1.44 ≤ v < 3.1.74 — worker is an ES module
 *   embedded in the main `.js`. `Module.postMessage` / `Module.postRun`
 *   still reach the engine. (Note: 3.1.60 has a separate upstream bug in
 *   its pthread detection but we treat it as the same generation here.)
 * - `ccall-only`: v ≥ 3.1.74 — `INCOMING_MODULE_JS_API` default dropped
 *   `postRun`/`print`/`printErr`/`preRun`, so Module-based dispatch is
 *   unreliable. We drive commands directly via `engine.ccall("usi_command",
 *   ...)` and tee stdout via a pthread prelude + main-thread console.log
 *   shim.
 */
export type LoaderGeneration = "classic-worker" | "esmodule-worker" | "ccall-only";

export function classifyVersion(version: string): LoaderGeneration {
  if (eq(version, "3.1.43")) return "classic-worker";
  if (lt(version, "3.1.74")) return "esmodule-worker";
  return "ccall-only";
}
