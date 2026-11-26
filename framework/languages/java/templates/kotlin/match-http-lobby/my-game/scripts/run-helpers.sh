# shellcheck shell=bash
e2e_classpath='e2e/build/install/e2e/lib/*'
java_flags=(--enable-native-access=ALL-UNNAMED -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8)

build_local() {
  mkdir -p .logs
  if (( $# == 0 )); then set -- installDist; fi
  if ! ./gradlew --no-daemon --max-workers=4 "$@" :e2e:javaHome >.logs/build.log 2>&1; then
    tail -n 35 .logs/build.log >&2
    return 1
  fi
}

local_java() {
  local java_home_path
  [[ -f .logs/java-home.txt && -d e2e/build/install/e2e/lib ]] || {
    echo "Build locally before using --no-build." >&2
    return 1
  }
  java_home_path=$(cat .logs/java-home.txt) || return 1
  [[ -x "$java_home_path/bin/java" ]] || return 1
  printf '%s\n' "$java_home_path/bin/java"
}

server_running() {
  local java
  [[ -f .logs/server.json ]] || return 1
  java=$(local_java) || return 1
  "$java" -cp "$e2e_classpath" MyGame.e2e.ServerHost --is-running
}
