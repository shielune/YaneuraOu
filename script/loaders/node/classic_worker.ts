// Node loader for emscripten 3.1.43.
//
// The build produces a separate `yaneuraou.<pkg>.worker.js` (classic script).
// `worker_shim.ts` eval's it into a worker_threads.Worker. Command + stdout
// routing uses the unified triple-tap (print callback, addMessageListener,
// worker stdout tee via postMessage) so the loader doesn't need to know
// which path a given emscripten generation prefers.

import type {
  EngineInstance,
  EngineOptions,
  Loader,
  LoaderContext,
} from "../types.ts";
import { eq } from "../version.ts";
import { installExternalQueue } from "../external_queue.ts";
import { instantiateWithUnifiedStdout } from "./common.ts";

const SHIM_URL = new URL("./worker_shim.ts", import.meta.url);

export const classicWorkerLoader: Loader = {
  name: "classic-worker/node",

  matches(version) {
    return eq(version, "3.1.43");
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
