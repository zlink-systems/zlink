#!/usr/bin/env bash
# 측정 전 확인: 벤치 바이너리가 지금 소스로 빌드됐는지. 하나라도 소스보다 오래됐으면 rc=1.
# 사용: bench-preflight.sh <worktree> <lang>...   (빌드 rc 확인은 호출하는 쪽 몫)
set -uo pipefail
R=$1; shift; G=$R/framework/bench/grpc; F=$R/framework/languages
echo "commit $(git -C $R rev-parse --short HEAD) dirty=$(git -C $R status --short -- framework bindings | wc -l)"
bad=0
for lang in "$@"; do
  case $lang in
    cpp)    arts="$G/cpp/build/bench_cpp_framework_server $G/cpp/build/bench_cpp_zlink_server $G/cpp/build/bench_cpp_client" ;;
    dotnet) arts="$G/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll $G/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll" ;;
    java)   arts="$G/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar" ;;
    node)   arts="$F/node/packages/framework/dist $F/node/packages/stream-connector/dist" ;;
  esac
  for a in $arts; do
    [ -e "$a" ] || { echo "MISSING $lang $a"; bad=1; continue; }
    # 가장 오래된 산출물보다 새 소스가 있으면 이전 빌드다.
    if [ -d "$a" ]; then
      # tsc -b는 바뀐 파일만 다시 쓰므로, 소스 파일마다 짝이 되는 dist 파일과 비교한다.
      src=${a%/dist}/src; stale=""
      while IFS= read -r ts; do
        js=$a/${ts#$src/}; js=${js%.ts}.js
        [ -e "$js" ] || continue  # tsconfig exclude 등 출력이 없는 파일
        [ "$js" -ot "$ts" ] && { stale="$ts"; break; }
      done < <(find "$src" -name '*.ts' -not -name '*.d.ts')
      printf '%s %s newest %s\n' $lang "${a#$R/}" "$(find "$a" -type f -printf '%TF_%TT\n' | sort | tail -1 | cut -c1-19)"
      [ -z "$stale" ] || { echo "STALE $lang: ${stale#$R/} is newer than its output"; bad=1; }
      continue
    fi
    ref=$a
    newer=$(find $F/$lang $G/$lang -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cs' -o -name '*.java' -o -name '*.kt' -o -name '*.ts' -o -name '*.mjs' -o -name '*.js' -o -name 'CMakeLists.txt' -o -name '*.csproj' -o -name '*.gradle.kts' \) \
      -not -path '*/build/*' -not -path '*/node_modules/*' -not -path '*/dist/*' -not -path '*/bin/*' -not -path '*/obj/*' -newer "$ref" -print -quit)
    printf '%s %s built %s\n' $lang "${a#$R/}" "$(date -r "$ref" +%F_%T)"
    [ -z "$newer" ] || { echo "STALE $lang: ${newer#$R/} is newer than ${ref#$R/}"; bad=1; }
  done
done
exit $bad
