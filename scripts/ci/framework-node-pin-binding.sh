#!/usr/bin/env bash
set -e
version="${1}"
export BINDING_VERSION="${version}"
node - <<'NODE'
const fs = require('node:fs');
const file = 'package.json';
const packageJson = JSON.parse(fs.readFileSync(file, 'utf8'));
packageJson.dependencies['@zlink-systems/zlink'] = `file:../../../.artifacts/wsl/npm/zlink-systems-zlink-${process.env.BINDING_VERSION}.tgz`;
fs.writeFileSync(file, `${JSON.stringify(packageJson, null, 2)}\n`);
NODE
BINDING_VERSION="${version}" npm install --package-lock-only --ignore-scripts --no-audit --no-fund
