# cfworkers パッケージのビルドを bun に切り替える計画

## 動機

- 全社的に bun に揃えたい（型チェックや biome 等のツール実行は npx ではなく bun に統一済み）
- 起動・install が npm より速い
- workflow 上の依存ステップを `bun install --frozen-lockfile` に置き換えれば再現性も向上

対象パッケージ:

- `yaneuraou-cfworkers/`
- `yaneuraou-mate-cfworkers/`

両方を同じ方針で揃える。npm publish 後の **配布物（dist tarball / npm tarball）は変更なし**。あくまで **ビルド時のツール** を npm/npx → bun に置き換えるだけ。

## 変更点

### 1. GitHub Actions workflow

`build-cfworkers.yml` / `build-mate-cfworkers.yml` の release ジョブ内:

```yaml
- name: Setup Node
  uses: actions/setup-node@v4
  with:
    node-version: "22"

- name: Compile TypeScript loader
  working-directory: yaneuraou-cfworkers
  run: |
    npm install --ignore-scripts
    npx tsc
```

を以下に置き換え:

```yaml
- name: Setup Bun
  uses: oven-sh/setup-bun@v2
  with:
    bun-version: latest

- name: Compile TypeScript loader
  working-directory: yaneuraou-cfworkers
  run: |
    bun install --frozen-lockfile
    bun tsc
```

`actions/setup-node` を消す。`bun tsc` は `node_modules/.bin/tsc` を呼ぶので tsc は引き続き必要（DevDependencies に残す）。

### 2. package.json scripts

```jsonc
"scripts": {
  "build:wasm": "act workflow_dispatch -W ../.github/workflows/build-cfworkers.yml --bind",
  "build:loader": "tsc",
  "build": "npm run build:wasm && npm run build:loader"
}
```

を:

```jsonc
"scripts": {
  "build:wasm": "act workflow_dispatch -W ../.github/workflows/build-cfworkers.yml --bind",
  "build:loader": "bun tsc",
  "build": "bun run build:wasm && bun run build:loader"
}
```

### 3. lockfile

- `package-lock.json` を削除
- `bun install` で `bun.lock` を生成
- `.gitignore` に `package-lock.json` を追加（誤って復活させないため）

### 4. publish パイプライン

現状は GitHub Release に tarball を載せるだけで npm publish はしていない。将来 npm publish するなら:

```yaml
- name: Publish to npm
  run: bun publish --access public
  env:
    NPM_CONFIG_TOKEN: ${{ secrets.NPM_TOKEN }}
```

（`bun publish` は npm registry にそのまま発行できる）

## 互換性チェック

| 項目 | 互換性 |
|---|---|
| TypeScript コンパイル | `bun tsc` は標準 tsc を呼ぶので結果は同一 |
| `@cloudflare/workers-types` | npm 経由で取得、bun でも問題なし |
| Workers ランタイム | 配布物は ES modules、bun/npm どちらでビルドしても挙動同じ |
| ローカル `npm run build:wasm` (act) | act 自体は変わらず、scripts 起点が `bun run` でも問題なし |

## 段階移行案

一度にやらず順次:

1. `yaneuraou-mate-cfworkers/` を新規パッケージなので **最初から bun** で組む
2. 動作確認できたら `yaneuraou-cfworkers/` を移行
3. CLAUDE.md / README に「ビルドは bun」と明記

## 懸念点

- **act + bun**: act runner image (catthehacker/ubuntu:full-latest) には bun が事前インストールされていないかも → setup-bun で必ず install されるようにする
- **bun.lock の互換性**: bun のメジャーバージョン跨ぎで lockfile 形式変わることあり → CI 側で `bun-version` を pin しておくと安全
- **既存ユーザー**: `yaneuraou-cfworkers` を install してる消費側は影響なし（配布物は dist/ だけ、build pipeline の変更は内部）

## やる順番

1. 本ドキュメントのレビュー・合意
2. `yaneuraou-mate-cfworkers` の `package.json` を bun script に書き換え
3. `build-mate-cfworkers.yml` を setup-bun に書き換え
4. tag `mate-cfworkers-v8.50.1` 等で test release してから本リリース
5. `yaneuraou-cfworkers` も同じ手順で移行（次の patch バージョンで）

## ロールバック

困ったら git revert で戻すだけ。配布物が変わらないので消費側影響なし。
