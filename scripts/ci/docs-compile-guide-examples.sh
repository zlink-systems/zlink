#!/usr/bin/env bash
set -e
if [ -d "$GITHUB_WORKSPACE/.artifacts/wsl/maven" ]; then
  ./gradlew :zlink-framework-doc-examples:compileJava
else
  echo "로컬 maven 저장소가 없어 건너뛴다 (systems.zlink:zlink 미공개)."
fi
