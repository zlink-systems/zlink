#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
cd "$ROOT"
mkdir -p "$ROOT/.logs"
. "$ROOT/scripts/run-helpers.sh"
# A scenario run uses the server started by run-server.sh when one is running.
mode="${arguments[0]:-}"
case "$mode" in
  --attach)
    if ! $local_mode; then exec node dist/e2e/runner.js "${arguments[@]}"; fi ;;
esac
if ! $local_mode && ! $build_only && [[ "$mode" != --check-* && "$mode" != --domain-checks-only ]] &&
  server_running "$ROOT"; then
  echo 'Using the running server (logs/server.json)'
  exec node dist/e2e/runner.js --attach "${arguments[@]}"
fi
if $local_mode; then
  if ! $no_build; then
    assert_no_running_server "$ROOT"
    if ! (npm ci && npm run build) >"$ROOT/.logs/build.log" 2>&1; then
      tail -n 20 "$ROOT/.logs/build.log" >&2
      exit 1
    fi
  fi
  if $build_only; then
    if [[ -f e2e/checks.conf ]]; then
      while read -r phase service command argument; do
        if [[ "$phase" == build && "$service" == runner ]]; then
          node dist/e2e/runner.js "$command" ${argument:+"$argument"}
        fi
      done < e2e/checks.conf
    fi
    exit 0
  fi
  exec node dist/e2e/runner.js "${arguments[@]}"
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
compose=(docker compose -f "$ROOT/docker-compose.yml" -p "$project")
service_lines=$(COMPOSE_PROFILES= "${compose[@]}" config --services)
redis_services=()
while IFS= read -r service; do
  redis_services+=("$service")
done <<< "$service_lines"
compose+=(--profile docker)
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
  "${compose[@]}" logs --no-color "${redis_services[@]}" >"$logs/redis.log" 2>&1 || result=1
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
run_checks() {
  local wanted="$1" phase service command argument
  [[ -f e2e/checks.conf ]] || return 0
  while read -r phase service command argument; do
    if [[ "$phase" == "$wanted" ]]; then
      "${compose[@]}" run --rm --no-deps -T "$service" "$command" ${argument:+"$argument"}
    fi
  done < e2e/checks.conf
}
if $build_only; then
  run_checks build
  exit 0
fi
case "$mode" in
  --check-structure|--domain-checks-only)
    "${compose[@]}" run --rm --no-deps -T --name "$project-check" runner "${arguments[@]}"
    exit 0 ;;
  --check-domain)
    if [[ -f e2e/checks.conf ]] && grep -q '^preflight ' e2e/checks.conf; then
      run_checks preflight
    else
      "${compose[@]}" run --rm --no-deps -T --name "$project-domain" runner "${arguments[@]}"
    fi
    exit 0 ;;
esac
"${compose[@]}" run --rm --no-deps -T --name "$project-check" runner --check-structure
run_checks preflight
"${compose[@]}" up -d --wait --wait-timeout 30 "${redis_services[@]}" >"$logs/redis-start.log" 2>&1
"${compose[@]}" run --rm --no-deps -T --name "$project-control" runner "${arguments[@]}"
