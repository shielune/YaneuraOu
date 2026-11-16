// Leading semicolon: same reason as in wasm_pre.js (this file is concatenated
// right after the emscripten preamble).
;
// Linked into the Node builds with pthreads only (see source/Makefile).
//
// emscripten's main thread parks an Atomics.waitAsync() on its own pthread
// mailbox and arms it again every time it wakes up (checkMailbox). V8 keeps
// a pending waitAsync() promise reachable, and that promise holds
// checkMailbox -> Module -> the shared WebAssembly.Memory. Module.terminate()
// (what dispose() calls) only stops the pthread workers; it never touches this
// wait. So after the last use the whole module, about 175-210 MB for KP256,
// can never be garbage collected.
//
// Hide waitAsync from the glue on the main thread. emscripten then wakes the
// main thread through postMessage instead (the path it already takes where
// Atomics.waitAsync does not exist), and no promise is left pending.
//
// This only shadows `Atomics` inside this module's scope: the global Atomics
// is untouched, and pthread workers keep the real one.
var Atomics = Module["ENVIRONMENT_IS_PTHREAD"]
  ? globalThis.Atomics
  : Object.create(globalThis.Atomics, { waitAsync: { value: undefined } });
