# __tests__

`script/loaders/` と `script/wasm_eval_{node,browser}.ts` を
デバッグするための最小再現・ライフサイクルトレース群。

実行は bun で直接:

```bash
bun __tests__/<name>.ts
```

これらは単体 test ランナー (bun test / vitest) に乗せた assertion test
では **ない**。各ファイルは「emscripten のライフサイクルコールバックが
どの順序で発火するか」「どの変種で `engine.postMessage` が drain
されるか」などを stderr に逐次出力する reproducer。期待する結果は
ファイル冒頭のコメントに書いてある。

一覧:

- `node_3.1.43_smoke.ts` — 3.1.43 node 変種で最小 USI (`usi` →
  `postMessage` → `addMessageListener`) が動くことを確認
- `node_3.1.70_lifecycle.ts` — 3.1.70 node 変種で preRun /
  onRuntimeInitialized / postRun / print / addMessageListener の発火
  タイミングを記録
- `node_3.1.70_noinit_ccall.ts` — `noInitialRun: true` で factory を
  呼び、`engine.ccall("usi_command", ...)` で USI コマンドを直接投げた
  ときの挙動を記録 (Options 未登録の usi は通るか検証)
- `node_3.1.70_callmain.ts` — `noInitialRun: true` + 明示
  `engine.callMain([])` で Options 初期化 + ccall のシーケンスを記録
- `node_3.1.70_external_queue.ts` — `script/loaders/external_queue.ts`
  を介して wasm_pre.js の queue を bypass したときの挙動を記録

これらの結果は `docs/wasm_eval_results.md` の「Node 側 3.1.60+ 共通の
未解決問題」節のエビデンスとして残す。
