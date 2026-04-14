// External command queue used as a drop-in replacement for wasm_pre.js's
// internal `Module.postMessage` queue when the latter fails to drain.
//
// Why this exists: wasm_pre.js installs a closure-scoped `queue` + `poll`
// pair, and the queue is only drained once `Module.postRun` fires. On
// emscripten ≥ 3.1.60 under Node, `postRun` never fires — emscripten's
// lifecycle calls `preRun` → `onRuntimeInitialized` → `main()`, and
// `main()` doesn't return (EXIT_RUNTIME=0 + YaneuraOu's USI REPL loop),
// so `postRun` is unreachable. On 3.1.43 it happens to work for other
// reasons, but the 3.1.60+ path needs an external pump.
//
// The pump drives `engine.ccall("usi_command", ...)` directly with the
// same backoff loop wasm_pre.js uses for its internal queue (ccall
// returns 1 = "busy, retry").

export interface PumpableEngine {
  ccall?: (
    name: string,
    returnType: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
  postMessage?: (cmd: string) => void;
}

export function installExternalQueue(engine: PumpableEngine): (cmd: string) => void {
  if (typeof engine.ccall !== "function") {
    throw new Error(
      "installExternalQueue: engine.ccall is missing — check EXPORTED_RUNTIME_METHODS",
    );
  }
  const queue: string[] = [];
  let polling = false;

  const pump = async () => {
    if (polling) return;
    polling = true;
    try {
      let backoff = 1;
      while (queue.length > 0) {
        const cmd = queue[0]!;
        const tryLater = engine.ccall!(
          "usi_command",
          "number",
          ["string"],
          [cmd],
        );
        if (tryLater) {
          await new Promise<void>((r) => setTimeout(r, backoff));
          backoff = Math.min(backoff * 2, 100);
        } else {
          queue.shift();
          backoff = 1;
        }
      }
    } finally {
      polling = false;
    }
  };

  const enqueue = (cmd: string) => {
    queue.push(cmd);
    void pump();
  };

  // Replace engine.postMessage so callers can keep using the old API, and
  // so wasm_pre.js's internal queue is bypassed even for anyone else who
  // still holds a reference to `engine.postMessage`.
  (engine as { postMessage?: (cmd: string) => void }).postMessage = enqueue;

  return enqueue;
}
