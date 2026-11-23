# shellcheck shell=bash
e2e_classpath='e2e/build/install/e2e/lib/*'

build_local() {
  echo 'Preparing: building the server (the first run downloads Gradle and dependencies and can take a few minutes)...'
  mkdir -p .logs
  if ! ./gradlew --no-daemon --max-workers=4 installDist :e2e:javaHome >.logs/build.log 2>&1; then
    tail -n 35 .logs/build.log >&2
    exit 1
  fi
  echo 'Build complete. Starting servers...'
}

local_java() {
  local java_home_path
  [[ -f .logs/java-home.txt && -d e2e/build/install/e2e/lib ]] || return 1
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

assert_no_running_server() {
  if server_running; then
    echo 'A server is already running. Stop it with Ctrl+C in its terminal before building.' >&2
    exit 1
  fi
}
