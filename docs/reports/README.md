# レポート

その時点の作業・調査・計測の記録。日付順。

継続的に参照する知見 (利用ガイド、リリース手順、エンジンオプション仕様など) は
`docs/` 直下に置く。ここに置くのは「いつ・何をして・何が分かったか」の記録。

| 日付 | 文書 | 内容 |
|---|---|---|
| 2026-04-14 | [`2026-04-14_wasm_emscripten_upgrade.md`](2026-04-14_wasm_emscripten_upgrade.md) | emscripten 3.1.43 → 5.0.5 追従。edge variant の新設、dual-runner 検証基盤 |
| 2026-07-27 | [`2026-07-27_nagisa_v3_diff_survey.md`](2026-07-27_nagisa_v3_diff_survey.md) | keinoda/YaneuraOu (NAGISA_V3) との差分調査。何が移植可能で何が V9.60 追従を要するか |
| 2026-07-28 | [`2026-07-28_wasm_v96x_port.md`](2026-07-28_wasm_v96x_port.md) | upstream V9.6x 追従。WASM 対応の復旧作業 |
| 2026-07-28 | [`2026-07-28_wasm_v96x_performance.md`](2026-07-28_wasm_v96x_performance.md) | V9.6x の性能測定。変種別スループット、スレッドスケーリング、node/browser 比較 |
| 2026-07-28 | [`2026-07-28_eval_comparison.md`](2026-07-28_eval_comparison.md) | 手持ち評価関数6種の depth 16 比較。V9.6x で全て読めることの確認を兼ねる |

## 補足

`2026-04-14_wasm_emscripten_upgrade.md` には「V9.x は upstream 都合で追従不可」という
当時の結論が含まれるが、これは **2026-07-28 の作業で覆っている**
([`2026-07-28_wasm_v96x_port.md`](2026-07-28_wasm_v96x_port.md) 参照)。
