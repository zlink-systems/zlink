#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .logs
if ! ./gradlew --no-daemon installDist :e2e:javaHome >.logs/build.log 2>&1; then
  tail -35 .logs/build.log
  exit 1
fi
java_home_path=$(cat .logs/java-home.txt)
exec "$java_home_path/bin/java" --enable-native-access=ALL-UNNAMED -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp 'e2e/build/install/e2e/lib/*' MyGame.e2e.ProgramKt "$@"
