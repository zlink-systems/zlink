#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
language="$1"
shift
case "$language" in
  java)
    classpath="${ZLINK_SESSION_BIND_JAVA_CLASSPATH:-$(cat "$ROOT/java-classpath.txt")}"
    exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" -cp "$classpath" \
      systems.zlink.framework.runtime.binding.SessionBindCrossLanguageHost "$@"
    ;;
  cpp) exec "${ZLINK_CPP_BUILD_DIR:-$ROOT/build}/zlink_cpp_session_bind_host" "$@" ;;
  *) echo "Unsupported Session bind peer: $language" >&2; exit 2 ;;
esac
