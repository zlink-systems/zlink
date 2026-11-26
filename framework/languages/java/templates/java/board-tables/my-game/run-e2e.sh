#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
local_mode=false
no_build=false
build_only=false
attach=false
check_structure=false
arguments=()
for argument in "$@"; do
  case "$argument" in
    --local) local_mode=true ;;
    --no-build) no_build=true ;;
    --build-only) build_only=true ;;
    --attach) attach=true ;;
    --check-structure) check_structure=true; arguments+=("$argument") ;;
    --scenario) arguments+=("$argument") ;;
    *) arguments+=("$argument") ;;
  esac
done
cd "$ROOT"
mkdir -p "$ROOT/.logs"
. "$ROOT/scripts/run-helpers.sh"
# A test run uses the server started by run-server.sh whenever one is running.
if ! $attach && ! $local_mode && ! $build_only && ! $check_structure && server_running; then
  echo 'Using the running server (.logs/server.json)'
  attach=true
fi
if $attach; then
  if $build_only || $check_structure; then
    echo '--attach requires a scenario run.' >&2
    exit 1
  fi
  if ! java=$(local_java); then
    echo 'Run ./run-server.sh to build and start the server before attaching.' >&2
    exit 1
  fi
  exec "$java" --enable-native-access=ALL-UNNAMED -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp "$e2e_classpath" MyGame.e2e.Program --attach "${arguments[@]}"
fi
if $local_mode; then
  if ! $no_build; then
    assert_no_running_server
    build_local
  fi
  $build_only && exit 0
  if ! java=$(local_java); then
    echo 'Build with ./run-e2e.sh --local --build-only before using --no-build.' >&2
    exit 1
  fi
  if ! $check_structure; then
    "$java" --enable-native-access=ALL-UNNAMED -cp 'server/table/build/install/table/lib/*' MyGame.table.Program --check-domain
  fi
  exec "$java" --enable-native-access=ALL-UNNAMED -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp "$e2e_classpath" MyGame.e2e.Program "${arguments[@]}"
fi
if $no_build; then
  echo '--no-build requires --local; Docker builds reuse the layer cache automatically.' >&2
  exit 1
fi
if [[ -S /var/run/docker.sock ]]; then
  runner_uid=$(id -u)
  runner_gid=$(id -g)
  MY_GAME_RUNNER_USER="$runner_uid:$runner_gid"
  MY_GAME_DOCKER_GID=$(stat -c '%g' /var/run/docker.sock 2>/dev/null || stat -f '%g' /var/run/docker.sock)
  export MY_GAME_RUNNER_USER MY_GAME_DOCKER_GID
fi
project="my-game-e2e-$(date +%s)-$$-$RANDOM"
logs="$ROOT/.logs/docker-$project"
mkdir -p "$logs"
# BuildKit reuses the previous transfer of a build context with the same directory name and
# compares only file size and modification time, so two projects named my-game with equal
# timestamps would build each other's files. A link with a per-run name gives every build its own
# transfer.
export MY_GAME_BUILD_CONTEXT="$logs/$project"
compose=(docker compose -f "$ROOT/docker-compose.yml" -p "$project" --profile docker)
cleanup() {
  local result=$?
  trap - EXIT
  rm -f "$MY_GAME_BUILD_CONTEXT"
  local containers
  containers=$(docker ps -aq --filter "label=my-game.e2e=$project") || result=1
  if [[ -n "$containers" ]]; then
    # IDs are Docker-generated hexadecimal tokens, one per line.
    # shellcheck disable=SC2086
    docker rm -f $containers >>"$logs/cleanup.log" 2>&1 || result=1
  fi
  "${compose[@]}" logs --no-color redis >"$logs/redis.log" 2>&1 || result=1
  "${compose[@]}" down --volumes >>"$logs/cleanup.log" 2>&1 || result=1
  for image in "$project-node" "$project-runner"; do
    if docker image inspect "$image" >/dev/null 2>&1; then
      docker image rm "$image" >>"$logs/cleanup.log" 2>&1 || result=1
    fi
  done
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "Docker logs: $logs"
ln -s "$ROOT" "$MY_GAME_BUILD_CONTEXT"
if ! "${compose[@]}" build >"$logs/build.log" 2>&1; then
  tail -n 30 "$logs/build.log" >&2
  exit 1
fi
$build_only && exit 0
"${compose[@]}" run --rm --no-deps -T --name "$project-check" runner --check-structure
$check_structure && exit 0
"${compose[@]}" run --rm --no-deps -T --entrypoint java --name "$project-domain" runner \
  --enable-native-access=ALL-UNNAMED -cp '/app/table/lib/*' MyGame.table.Program --check-domain
"${compose[@]}" up -d --wait --wait-timeout 30 redis >"$logs/redis-start.log" 2>&1
"${compose[@]}" run --rm --no-deps -T --name "$project-control" runner "${arguments[@]}"
