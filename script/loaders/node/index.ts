import type { Loader } from "../types.ts";
import { classicWorkerLoader } from "./classic_worker.ts";
import { esmoduleWorkerLoader } from "./esmodule_worker.ts";
import { ccallOnlyLoader } from "./ccall_only.ts";

/**
 * Registered in priority order — the first match wins. Keep more specific
 * version predicates (e.g. exact 3.1.43) before broader ranges.
 */
export const nodeLoaders: readonly Loader[] = [
  classicWorkerLoader,
  esmoduleWorkerLoader,
  ccallOnlyLoader,
];

export { classicWorkerLoader, esmoduleWorkerLoader, ccallOnlyLoader };
