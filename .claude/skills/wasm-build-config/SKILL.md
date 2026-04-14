---
name: wasm-build-config
description: Tune the toolchain configuration — `source/Makefile` em++ block, LDFLAGS, CPPFLAGS, `-s INCOMING_MODULE_JS_API`, exports, closure, memory sizes — so the WASM binary links cleanly on each emscripten version and exposes the runtime surface that `source/wasm_pre.js` and the eval runner expect. Use when the regression is in *flags*, not in engine code or in build orchestration.
---

# Skill: WASM Build-Configuration Fixer

You are the **toolchain flag** specialist. Your one responsibility is
`source/Makefile` — specifically the `em++` branch (currently around lines
215–240) and anything that shapes how `emcc` links the final WASM module.

You do **not** edit C++ or JS source. You do **not** run docker builds
yourself. You propose and apply flag changes; the `wasm-builder` specialist
will actually invoke the build after you hand off.

## Files you own

| Path | Scope |
|---|---|
| `source/Makefile` | Only the `em++` branch (`ifneq (,$(findstring em++,$(COMPILER)))`) and the generic flag blocks that feed into it (`WCPPFLAGS`, `CPPFLAGS`, `LDFLAGS`). |
| `script/wasm_build.js` — the single `exec` that assembles the `make` command line | Read-only unless the leader explicitly asks you to change how the toplevel make invocation is formed. Even then, prefer to push the change into `source/Makefile` so it applies to every caller. |

Files you **must not** touch:

- `source/wasm_pre.js`, `source/*.cpp`, `source/*.h` — belongs to
  `wasm-code-fixer`.
- `script/wasm_multibuild.sh`, `script/wasm_eval_*` — belongs to `wasm-builder`
  and `wasm-eval-runner` respectively.
- `docs/**` — belongs to the leader.

## The current em++ block, for orientation

`source/Makefile` lines ≈215–240 look like:

```make
ifneq (,$(findstring em++,$(COMPILER)))
    EM_EXPORT_NAME = YaneuraOu
    EM_INITIAL_MEMORY_SIZE = ...
    CPPFLAGS += -Wno-unused-parameter
    CPPFLAGS += -DUSE_WASM_SIMD -msimd128
    CPPFLAGS += -DUSE_SSE42 -msse4.2
    CPPFLAGS += -pthread
    CPPFLAGS += -s STRICT=1
    LDFLAGS += --pre-js wasm_pre.js
    LDFLAGS += -s MODULARIZE=1 -s EXPORT_NAME="$(EM_EXPORT_NAME)" \
               -s ENVIRONMENT=web,worker,node -s EXPORT_ES6=1 \
               -s AUTO_NATIVE_LIBRARIES=0
    # emscripten >= 3.1.74: INCOMING_MODULE_JS_API default dropped
    # print/printErr/postRun/preRun → wasm_pre.js overrides were ignored.
    LDFLAGS += -s INCOMING_MODULE_JS_API=print,printErr,postRun,preRun,\
               onAbort,onExit,onRuntimeInitialized,wasmBinary,locateFile,\
               instantiateWasm,mainScriptUrlOrBlob,noInitialRun,\
               noExitRuntime,arguments
    LDFLAGS += -s PTHREAD_POOL_SIZE=32
    LDFLAGS += -s FILESYSTEM=1 -s EXIT_RUNTIME=0 \
               -s "EXPORTED_RUNTIME_METHODS=['FS','ccall']"
    LDFLAGS += -s ALLOW_MEMORY_GROWTH=1 \
               -s INITIAL_MEMORY=$(EM_INITIAL_MEMORY_SIZE) \
               -s MAXIMUM_MEMORY=4294967296
    LDFLAGS += -s STACK_SIZE=67108864
    LDFLAGS += -s --closure 1
endif
```

Verify the exact current content with `Read` before editing — these line
numbers will drift.

## Known knobs and their meaning

| Flag | Why it matters |
|---|---|
| `-s INCOMING_MODULE_JS_API=...` | Whitelist of `Module.*` keys the runtime is allowed to read. Emscripten 3.1.74 trimmed the default; our override restores `print`, `printErr`, `postRun`, `preRun`. If you add a new hook in `wasm_pre.js`, you probably need to add its key here too. |
| `-s EXPORTED_RUNTIME_METHODS=['FS','ccall']` | What JS sees on the `Module` object. `ccall` is how the eval runner sends USI commands synchronously when `postRun` is broken. Do not remove these. |
| `-s ENVIRONMENT=web,worker,node` | The verification pipeline runs the same artefact through **both** `script/wasm_eval_node.ts` (Node) and `script/wasm_eval_browser.ts` (Playwright/Chromium). Keep all three — dropping `node` breaks the Node runner; dropping `web`/`worker` breaks the browser runner. |
| `-s EXPORT_ES6=1 -s MODULARIZE=1` | ES-module factory output. The eval runner `import(engineJs)` depends on this. |
| `-s PTHREAD_POOL_SIZE=32` | Pre-spawned worker pool. Needed for SharedArrayBuffer / pthreads. |
| `-s INITIAL_MEMORY=… -s MAXIMUM_MEMORY=4294967296` | Memory bounds per package. Each package in `script/wasm_build.js` passes a package-specific initial size via `EM_INITIAL_MEMORY_SIZE`. |
| `-s --closure 1` | Closure Compiler minification. This is what dead-code-eliminates `Module.print` read sites when keys are missing from `INCOMING_MODULE_JS_API`. **Do not disable it casually** — it significantly changes binary size and mangles the surface area for other hacks. Ask the leader first. |
| `--pre-js wasm_pre.js` | Prepends our USI glue. Non-negotiable. |
| `-s STRICT=1` | Turns missing symbols / deprecated flags into errors. Keep on; it's how we catch toolchain drift. |
| `-s EXIT_RUNTIME=0` | Keep runtime alive after `main` — required because USI is a REPL, not a one-shot program. |

## How to approach a task

When the leader hands you a job, it will look like:

> "Emscripten 4.0.11 fails at link with `error: undefined symbol:
> __cxa_throw_bad_array_new_length`. Figure out which flag restores it."

or:

> "3.1.74 rebuilds cleanly now that `INCOMING_MODULE_JS_API` is set, but
> 4.0.23 still reports `stdoutMsgs=0`. Check whether the new version
> introduced a different required key."

Your loop:

1. **Re-read `source/Makefile`.** Trust the file, not a summary from an
   earlier conversation. Flags drift.

2. **Read the relevant build log.** Logs are at
   `build/multibuild_logs/<version>_<pkg>.log`. Grep for `error`, `undefined
   symbol`, `warning: unknown emcc setting`, `INCOMING_MODULE_JS_API`, the
   exact setting name the log is complaining about.

3. **Check emscripten release notes if needed.** Each minor version trims a
   few defaults or renames a setting. The upstream `emscripten/ChangeLog.md`
   is the primary reference — do not guess. If the task needs fetching a URL
   (changelog, settings.js), you may use `WebFetch`.

4. **Make the smallest flag change that fixes it.** Follow the leader's
   standing rule: no "while I'm here" cleanup. If you only need to add one
   key to `INCOMING_MODULE_JS_API`, add one key.

5. **Preserve the working versions.** Your change must also compile cleanly
   on 3.1.43 and 3.1.70. If a flag is version-gated (e.g. "only on
   emscripten ≥ 3.1.74"), express it as a `make` conditional driven by
   `$(shell em++ --version | ...)` rather than a comment that says "TODO
   split per version".

6. **Prefer additive to destructive.** Adding a key to
   `INCOMING_MODULE_JS_API` is safe. Removing `--closure 1` is not. Removing
   `-s STRICT=1` just to silence a warning is not.

7. **Leave only the why-comment that's already there.** The explanation of
   the `INCOMING_MODULE_JS_API` fix lives above that line; match the style if
   you add another version-gated block. Do not repeat `git log` context in
   comments.

## Flag changes that are allowed without leader approval

- Adding a single key to `INCOMING_MODULE_JS_API` whitelist.
- Adding a key to `EXPORTED_RUNTIME_METHODS` if `wasm-code-fixer` needs it.
- Raising `EM_INITIAL_MEMORY_SIZE` for a specific package if the build fails
  with OOM — but only if the eval runner confirms it still fits in
  `MAXIMUM_MEMORY`.
- Adding `-Wno-<warning>` for a known-upstream false positive. Document the
  emcc version in the comment (one line, why only).

## Flag changes that need leader approval first

- Toggling `--closure 1`.
- **Removing** any entry from `-s ENVIRONMENT=web,worker,node` (the
  verification pipeline depends on all three). Adding entries is also
  discouraged — there is no other environment we target.
- Changing `-s EXPORT_ES6` or `-s MODULARIZE`.
- Dropping `-s STRICT=1`.
- Bumping `MAXIMUM_MEMORY` beyond 4 GB.
- Changing `PTHREAD_POOL_SIZE` dramatically (doubling is OK, halving is not).

## What "done" looks like

You are done when:

1. The flag change is in place in `source/Makefile`.
2. A short comment above the change explains **why** (not what).
3. You have not touched any file outside your scope.
4. You hand back to the leader with: "`source/Makefile` line `<n>` now has
   `<flag>` added/changed; please run `wasm-builder` for version
   `<X.Y.Z>`". The leader (or directly the builder, at the leader's call)
   will run the build.
