// Node loader for emscripten 3.1.44 ≤ v < 3.1.74.
//
// The worker entry on this generation is the main `yaneuraou.<pkg>.js`
// itself, re-imported as an ES module. Command + stdout routing uses the
// unified triple-tap from common.ts so we don't depend on whether this
// specific version still invokes `Module.postRun` or not.

import type {
  EngineInstance,
  EngineOptions,
  Loader,
  LoaderContext,
} from "../types.ts";
import { gte, lt } from "../version.ts";
import { instantiateWithUnifiedStdout } from "./common.ts";

const SHIM_URL = new URL("./worker_shim.ts", import.meta.url);

export const esmoduleWorkerLoader: Loader = {
  name: "esmodule-worker/node",

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

    const engine = await instantiateWithUnifiedStdout(ctx, SHIM_URL, push);

    if (typeof engine.ccall !== "function") {
      throw new Error(
        "esmodule-worker/node: engine.ccall is missing — check EXPORTED_RUNTIME_METHODS",
      );
    }

    return {
      sendCommand(cmd: string) {
        engine.ccall!("usi_command", "number", ["string"], [cmd]);
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
