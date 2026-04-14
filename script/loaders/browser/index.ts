import type { Loader } from "../types.ts";
import { classicWorkerLoader } from "./classic_worker.ts";
import { esmoduleWorkerLoader } from "./esmodule_worker.ts";
import { ccallOnlyLoader } from "./ccall_only.ts";

/**
 * Browser-side loader registry — same priority ordering as the Node side.
 * `runner.html` asks `pickLoader` from ../detect.ts to match the version
 * that `wasm_eval_browser.ts` passes in as a URL query parameter.
 */
export const browserLoaders: readonly Loader[] = [
  classicWorkerLoader,
  esmoduleWorkerLoader,
  ccallOnlyLoader,
];

export { classicWorkerLoader, esmoduleWorkerLoader, ccallOnlyLoader };
