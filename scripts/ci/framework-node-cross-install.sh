#!/usr/bin/env bash
set -e
bash ../../../scripts/ci/framework-node-pin-binding.sh "$1"
bash ../../../scripts/ci/framework-node-install.sh
npm run build
