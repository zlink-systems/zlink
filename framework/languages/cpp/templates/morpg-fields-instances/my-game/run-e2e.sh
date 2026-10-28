#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
local_mode=false
no_build=false
build_only=false
arguments=()
for argument in "$@"; do
  case "$argument" in
    --local) local_mode=true ;;
    --no-build) no_build=true ;;
    --build-only) build_only=true ;;
    *) arguments+=("$argument") ;;
  esac
done
mkdir -p "$ROOT/.logs"
if $local_mode; then
  if ! $no_build; then
    if ! { cmake -P bootstrap.cmake && cmake --preset default && cmake --build --preset default; } >"$ROOT/.logs/build.log" 2>&1; then
      tail -n 20 "$ROOT/.logs/build.log" >&2
      exit 1
    fi
  fi
  $build_only && exit 0
  exec "$ROOT/build/bin/my-game-e2e" --root "$ROOT" "${arguments[@]}"
fi
if $no_build; then
  echo '--no-build requires --local; Docker builds reuse the layer cache automatically.' >&2
  exit 1
fi
if [[ -S /var/run/docker.sock ]]; then
  export MY_GAME_RUNNER_USER="$(id -u):$(id -g)"
  export MY_GAME_DOCKER_GID="$(stat -c '%g' /var/run/docker.sock 2>/dev/null || stat -f '%g' /var/run/docker.sock)"
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
"${compose[@]}" run --rm --no-deps -T --name "$project-check" runner "${arguments[@]}" --check-structure
for argument in "${arguments[@]}"; do
  [[ "$argument" == "--check-structure" || "$argument" == "--check-domain" ]] && exit 0
done
"${compose[@]}" up -d --wait --wait-timeout 30 redis >"$logs/redis-start.log" 2>&1
"${compose[@]}" run --rm --no-deps -T --name "$project-control" runner "${arguments[@]}"
