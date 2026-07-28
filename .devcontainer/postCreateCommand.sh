#!/bin/zsh

sudo chown -R vscode:vscode node_modules
bun install --frozen-lockfile --ignore-scripts

# ⚠ install-deps は OS 側の共有ライブラリを入れるだけで、ブラウザ本体は落とさない。
#   本体は install で入れる。--ignore-scripts を付けている以上 package.json の
#   postinstall は走らないので、ここで明示的に呼ぶ必要がある。
bunx playwright install-deps chromium
bunx playwright install chromium
