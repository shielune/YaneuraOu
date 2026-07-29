#!/bin/zsh

sudo chown -R $(whoami):$(whoami) node_modules
bun install --frozen-lockfile --ignore-scripts

bunx playwright install-deps chromium
bunx playwright install chromium
