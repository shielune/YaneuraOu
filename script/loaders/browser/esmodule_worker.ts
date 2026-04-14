// Browser loader for emscripten 3.1.44 ≤ v < 3.1.74. Uses the unified
// triple-tap so we're covered regardless of which stdout path the
// generated JS actually takes inside Chromium.

import type {
  EngineInstance,
  EngineOptions,
  Loader,
  LoaderContext,
} from "../types.ts";
import { gte, lt } from "../version.ts";
import { loadEngineWithUnifiedStdout } from "./common.ts";

export const esmoduleWorkerLoader: Loader = {
  name: "esmodule-worker/browser",

  matches(version) {
    return gte(version, "3.1.44") && lt(version, "3.1.74");
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

    if (typeof engine.postMessage !== "function") {
      throw new Error(
        "esmodule-worker/browser: engine.postMessage is missing — wasm_pre.js wiring failed",
      );
    }

    return {
      async sendCommand(cmd: string) {
        engine.postMessage!(cmd);
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
