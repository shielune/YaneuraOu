// Node loader for emscripten v ≥ 3.1.74.
//
// Same unified triple-tap as the other generations; the main reason this
// loader exists as a separate file is to keep the `matches` range and the
// name explicit in the registry. The actual wiring is shared with
// classic/esmodule workers.

import type {
  EngineInstance,
  EngineOptions,
  Loader,
  LoaderContext,
} from "../types.ts";
import { gte } from "../version.ts";
import { ccallWithRetry } from "../retry.ts";
import { instantiateWithUnifiedStdout } from "./common.ts";

const SHIM_URL = new URL("./worker_shim.ts", import.meta.url);

export const ccallOnlyLoader: Loader = {
  name: "ccall-only/node",

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

    const engine = await instantiateWithUnifiedStdout(ctx, SHIM_URL, push);

    if (typeof engine.ccall !== "function") {
      throw new Error(
        "ccall-only/node: engine.ccall is missing — are EXPORTED_RUNTIME_METHODS correct?",
      );
    }

    return {
      async sendCommand(cmd: string) {
        await ccallWithRetry(
          () => engine.ccall!("usi_command", "number", ["string"], [cmd]),
          cmd,
        );
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
