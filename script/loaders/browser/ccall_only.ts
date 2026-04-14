// Browser loader for emscripten v ≥ 3.1.74. Uses the unified triple-tap;
// same wiring as the older generations, just a different `matches` range.
// See docs/wasm_eval_results.md for why this bucket exists as a separate
// file even though the runtime code path is identical.

import type {
  EngineInstance,
  EngineOptions,
  Loader,
  LoaderContext,
} from "../types.ts";
import { gte } from "../version.ts";
import { installExternalQueue } from "../external_queue.ts";
import { loadEngineWithUnifiedStdout } from "./common.ts";

export const ccallOnlyLoader: Loader = {
  name: "ccall-only/browser",

  matches(version) {
    return gte(version, "3.1.74");
  },

  async load(
    ctx: LoaderContext,
    _opts: EngineOptions = {},
  ): Promise<EngineInstance> {
    const buffered: string[] = [];
    const listeners: Array<(line: string) => void> = [];
    const push = (s: string) => {
      buffered.push(s);
      for (const fn of listeners) fn(s);
    };

    const engine = await loadEngineWithUnifiedStdout(ctx, push);

    const enqueue = installExternalQueue(engine);

    return {
      async sendCommand(cmd: string) {
        enqueue(cmd);
      },
      onLine(listener) {
        listeners.push(listener);
        for (const l of buffered) listener(l);
      },
      async dispose() {
        try {
          engine.terminate?.();
        } catch {
          /* ignore */
        }
      },
    };
  },
};
