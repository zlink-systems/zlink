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
  mkdir -p "$out_dir"
  out_dir="$(cd "$out_dir" && pwd -P)"
  (
    cd "$repo_root/framework/languages/node"
    npx tsc -b packages/http-client
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
