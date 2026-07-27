// Tiny retry helper used by every loader's `sendCommand` implementation.
// Extracted into its own module so both the runners (via wasm_eval_common)
// and the loaders can import it without introducing a circular dependency
// between loaders and the USI flow driver.

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

/**
 * Call `ccall()` in a backoff loop until it returns 0 ("command accepted")
 * or the deadline expires. A return value of 1 means "engine is busy, try
 * again later" — this is what `wasm_pre.js`'s USI command queue returns
 * while the engine is still processing an earlier command.
 */
export async function ccallWithRetry(
  call: () => number,
  cmdLabel: string,
  timeoutMs = 5000,
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let backoff = 1;
  while (Date.now() < deadline) {
    const tryLater = call();
    if (!tryLater) return;
    await sleep(backoff);
    backoff = Math.min(backoff * 2, 200);
  }
  throw new Error(`sendCommand busy timeout: ${cmdLabel}`);
}
