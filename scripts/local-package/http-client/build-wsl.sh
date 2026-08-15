#!/usr/bin/env bash
set -euo pipefail

# Builds local packages for the zlink HTTP client (framework component, not a binding).
# Policy: e2e/sample consumers reference a pinned local package version instead of the
# http-client source, so the client can be modified without breaking them. See
# framework/doc/http-client/http-client-unification-plan.ko.md (Phase 2).
#
# cpp is intentionally excluded: the static library has a PUBLIC dependency on the
# in-tree zlink::framework headers, so mixing an installed snapshot with in-tree
# framework sources risks ODR violations. cpp consumers keep source references and
# are gated by the http-client contract tests instead.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"

mkdir -p "$artifact_root"

if [ "$#" -eq 0 ]; then
  set -- dotnet java node
fi

build_dotnet() {
  local out_dir="$artifact_root/nuget"
  mkdir -p "$out_dir"
  dotnet pack "$repo_root/framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/Systems.Zlink.Stream.Connector.csproj" \
    -c "$configuration" -o "$out_dir"
  dotnet pack "$repo_root/framework/languages/dotnet/src/Zlink.Framework.Contracts/Zlink.Framework.Contracts.csproj" \
    -c "$configuration" -o "$out_dir"
  dotnet pack "$repo_root/framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj" \
    -c "$configuration" -o "$out_dir"
  echo "-- http-client .NET package output: $out_dir"
}

build_java() {
  local out_dir="$artifact_root/maven"
  mkdir -p "$out_dir"
  out_dir="$(cd "$out_dir" && pwd -P)"
  (
    cd "$repo_root/framework/languages/java"
    MAVEN_REPOSITORY_URL="file://$out_dir" ./gradlew \
      :zlink-http-client:publish \
      :zlink-http-client-kotlin:publish
  )
  echo "-- http-client Java/Kotlin Maven output: $out_dir"
}

build_node() {
  local out_dir="$artifact_root/npm"
  local zlink_package="$out_dir/zlink-systems-zlink-$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION").tgz"
  mkdir -p "$out_dir"
  out_dir="$(cd "$out_dir" && pwd -P)"
  [[ -f "$zlink_package" ]] || {
    echo "Node Core binding package is required before http-client packaging: $zlink_package" >&2
    exit 1
  }
  (
    cd "$repo_root/framework/languages/node"
    compiler="${repo_root}/framework/languages/node/node_modules/typescript/bin/tsc"
    bootstrap_dir=""
    bootstrap_linked=0
    cleanup_bootstrap() {
      if [[ "$bootstrap_linked" -eq 1 ]]; then
        rm -f "${repo_root}/framework/languages/node/node_modules"
      fi
      if [[ -n "$bootstrap_dir" ]]; then
        rm -rf "$bootstrap_dir"
      fi
    }
    trap cleanup_bootstrap EXIT
    if [[ ! -x "$compiler" ]]; then
      # The workspace cannot run npm ci until this tarball exists because it is
      # a local file dependency. Bootstrap only the compiler and type/runtime
      # packages in an ignored artifact directory for this first package.
      bootstrap_dir="$(mktemp -d "$artifact_root/node-http-bootstrap.XXXXXX")"
      npm install --prefix "$bootstrap_dir" --no-save --no-package-lock \
        --ignore-scripts --no-audit --no-fund \
        typescript@5.8.3 @types/node@22.15.30 \
        @opentelemetry/api@1.9.0 @opentelemetry/api-logs@0.221.0 \
        undici@6.27.0 "$zlink_package"
      ln -s "$bootstrap_dir/node_modules" \
        "${repo_root}/framework/languages/node/node_modules"
      bootstrap_linked=1
      compiler="${repo_root}/framework/languages/node/node_modules/typescript/bin/tsc"
    fi
    "$compiler" -b packages/http-client
    npm pack --pack-destination "$out_dir" ./packages/http-client
  )
  echo "-- http-client Node tarball output: $out_dir"
}

for lang in "$@"; do
  case "$lang" in
    dotnet) build_dotnet ;;
    java) build_java ;;
    node) build_node ;;
    cpp)
      echo "cpp is excluded from http-client local packaging (ODR policy; see header comment)" >&2
      exit 2
      ;;
    *)
      echo "Unknown language: $lang" >&2
      echo "Usage: $0 [dotnet] [java] [node]" >&2
      exit 2
      ;;
  esac
done

echo "-- http-client local packages are under $artifact_root"
