#!/usr/bin/env bash
set -euo pipefail

job=${MEASURE_JOB_DIR:-/root/cxjobs/measplan}
repo=${MEASURE_REPO:-/root/wt/measplan}
branch=${MEASURE_BRANCH:-perf/measure-plan}
historical=/mnt/nas/work/logs/studio/c-baseline-1128-final
run_one=/root/perf-runs/run-one.sh
core=/root/.cache/zlink/core/1.26.0/linux-x64
runtime=$core/lib/libzlink.so.1.26.0
runner_path=/root/toolchains/venv/bin:/root/toolchains/cargo/bin:/root/toolchains/go/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# run-one re-enters this same script (the measured commit's copy) for the locked runner.
self=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
table=$(dirname "$self")/table.py
patterns=(DEALER_DEALER DEALER_ROUTER_SENDSEND ROUTER_ROUTER_SENDSEND
          DEALER_ROUTER_REQREP ROUTER_ROUTER_REQREP PUBSUB)
c_names=(dealer_dealer dealer_router_sendsend router_router_sendsend
         dealer_router_reqrep router_router_reqrep pubsub)
binding_names=(dealer_dealer dealer_router router_router
               dealer_router_reqrep router_router_reqrep pubsub)
languages=(C Rust Go Python C++ .NET)
slugs=(c rust go python cpp dotnet)
sizes=64,256,1024,4096,65536,131072

fail() { printf '%s\n' "$*" >&2; exit 1; }

preflight() {
    local load mem
    read -r load _ < /proc/loadavg
    mem=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    printf 'preflight load_1m=%s MemAvailable_KiB=%s\n' "$load" "$mem"
    awk -v l="$load" -v m="$mem" 'BEGIN {exit !(l != "" && l < 12 && m >= 10485760)}'
}

# run-one invokes this entry after obtaining its lock and verifying CPU frequency.
if [[ ${1:-} == --execute && ${2:-} == --locked-runner ]]; then
    [[ ${RUN_ONE_LOCKED:-} == 1 && $# -ge 4 ]] || fail "run-one lock is required"
    shift 2
    preflight || exit 75
    exec "$@"
fi

case "${1:---dry-run}" in
    --dry-run) execute=0 ;;
    --execute) execute=1 ;;
    -h|--help) printf 'Usage: %s [--dry-run | --execute]\n' "$0"; exit 0 ;;
    *) fail "Unknown option: $1" ;;
esac
(( $# <= 1 )) || fail "Use one mode only"

cd "$repo"
[[ $(git branch --show-current) == "$branch" ]] || fail "Expected branch $branch"
head=$(git rev-parse HEAD)
id="measplan-$(date -u +%Y%m%dT%H%M%SZ)-$$"
campaign=$job/$id
stage=$campaign/source
index=$campaign/runs.tsv
rust_target=$campaign/rust-multi
go_build=$campaign/go-multi
c_build=$stage/bindings/c/build-release-1.26.0
cpp_build=$stage/bindings/cpp/build
project=$stage/bindings/dotnet/perf/multi/Zlink.BindingBench.Multi
py_native=$stage/bindings/python/src/zlink/native/linux-x86_64
runners=(bindings/c/perf/run_benchmarks_multi.sh
         bindings/rust/perf/run_benchmarks_multi.sh
         bindings/go/perf/run_benchmarks_multi.sh
         bindings/python/perf/run_benchmarks_multi.sh
         bindings/cpp/perf/run_binding_multi.sh
         bindings/dotnet/perf/multi/run_benchmarks.sh)

command_line() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

step() {
    command_line "$@"
    if (( execute )); then "$@"; fi
}

check() {
    local test_flag=$1 path=$2
    if test "$test_flag" "$path"; then
        printf 'OK %s\n' "$path"
    else
        printf 'MISSING %s\n' "$path"
        return 1
    fi
}

artifact() {
    if ! check "$@"; then
        if (( execute )); then fail "Build artifact is missing: $2"; fi
    fi
}

printf 'HEAD=%s branch=%s campaign=%s\n' "$head" "$branch" "$campaign"
for file in "$run_one" "$table" "${runners[@]/#/$repo/}" \
    "$repo/bindings/python/perf/multi/run_benchmarks.sh" \
    /root/toolchains/venv/bin/python3 /root/toolchains/cargo/bin/cargo /root/toolchains/go/bin/go \
    /root/toolchains/rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc; do
    check -x "$file"
done
for bin in bash git cmake dotnet flock taskset cpupower; do
    check -x "$(PATH=$runner_path command -v "$bin")"
done
for file in "$core/include/zlink.h" "$runtime" "$core/lib/libzlink.so" \
    "$core/share/zlink/core-package-provenance.json" /root/jobs/fx/cx/measure-x.lock; do
    check -f "$file"
done
printf '%s  %s\n' 569c3ec6d882f9f2e35c14166acaece29fae2ad845f08c36a13343e42113cd5a "$runtime" | sha256sum -c -
if (( ! execute )); then
    for budget in 32 64 256; do
        burst=/root/cxjobs/c-burst/build-${budget}k
        check -f "$burst/CMakeCache.txt"
        for pattern in dealer_dealer dealer_router_sendsend router_router_sendsend; do
            for role in server client; do check -x "$burst/perf/comp_src_${pattern}_$role"; done
        done
    done
fi

unstable=$("$table" unstable "$historical" --tsv)
printf 'Historical unstable cells (%s):\n%s\n' "$(wc -l <<< "$unstable")" "$unstable"
[[ ! -e "$campaign" ]] || fail "Campaign already exists: $campaign"
step mkdir "$campaign"
step mkdir -p "$campaign/home" "$campaign/tmp" "$campaign/cargo"
step git clone --quiet --shared --single-branch --branch "$branch" -- "$repo" "$stage"
step cp -a --reflink=auto /root/toolchains/cargo/registry "$campaign/cargo/"
# NuGet restores offline from a copy of the local cache; the isolated HOME has no package source.
step cp -a --reflink=auto /root/.nuget/packages "$campaign/nuget"
if (( execute )); then
    [[ $(git -C "$stage" rev-parse HEAD) == "$head" ]] || fail "Source HEAD changed during staging"
    : > "$index"
fi

# Use the installed toolchains and isolate new caches in the campaign directory.
common_env=(env -i "PATH=$runner_path" "HOME=$campaign/home" "TMPDIR=$campaign/tmp"
            LANG=C.UTF-8 PIN_KHZ=2000000 PIN_ROLES=0
            ZLINK_CORE_SOURCE=release ZLINK_CORE_RELEASE_VERSION=1.26.0
            "ZLINK_CORE_PACKAGE_PREFIX=$core"
            PERF_CTX_AUTO_HWM_ENABLE=1 PERF_MULTI_RUN_COOLDOWN_MS=3000
            DOTNET_CLI_USE_MSBUILD_SERVER=0 UseSharedCompilation=false
            MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
            "DOTNET_CLI_HOME=$campaign/home" "NUGET_PACKAGES=$campaign/nuget"
            PYTHONDONTWRITEBYTECODE=1)
rust_env=("CARGO_HOME=$campaign/cargo" RUSTUP_HOME=/root/toolchains/rustup
          RUSTUP_TOOLCHAIN=stable-x86_64-unknown-linux-gnu
          "ZLINK_RUST_NATIVE_DIR=$core/lib" "CARGO_TARGET_DIR=$rust_target")
go_env=(CGO_ENABLED=1 "CGO_CFLAGS=-I$core/include"
        "CGO_LDFLAGS=-L$core/lib -Wl,-rpath,$core/lib" "ZLINK_GO_NATIVE_DIR=$core/lib"
        "GOPATH=$campaign/go" "GOCACHE=$campaign/go-cache" "GOTMPDIR=$campaign/tmp" GOTOOLCHAIN=local)
python_env=("PYTHON_EXECUTABLE=/root/toolchains/venv/bin/python3"
            "ZLINK_LIBRARY_PATH=$py_native/libzlink.so.1.26.0")

c_targets=()
cpp_targets=()
rust_targets=()
for n in "${!c_names[@]}"; do
    for role in server client; do
        c_targets+=("comp_src_${c_names[n]}_$role")
        cpp_targets+=("cpp_comp_src_${binding_names[n]}_$role")
        rust_targets+=("perf_multi_${binding_names[n]}_$role")
    done
done
build=(nice -n 10 "${common_env[@]}")
step "${build[@]}" cmake -S "$stage/bindings/c" -B "$c_build" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DZLINK_C_BUILD_BENCHMARKS=ON \
    -DZLINK_C_BUILD_SAMPLES=OFF "-DZLINK_CORE_DIR=$stage/core" "-DZLINK_C_CORE_BUILD_DIR=$core"
step "${build[@]}" cmake --build "$c_build" --parallel 8 --target "${c_targets[@]}"
artifact -f "$c_build/CMakeCache.txt"
for target in "${c_targets[@]}"; do artifact -x "$c_build/perf/$target"; done

step "${build[@]}" "${rust_env[@]}" cargo build --release --locked --jobs 8 \
    --manifest-path "$stage/bindings/rust/perf/multi/Cargo.toml"
for target in "${rust_targets[@]}"; do artifact -x "$rust_target/release/$target"; done

step mkdir "$go_build"
step "${build[@]}" "${go_env[@]}" go -C "$stage/bindings/go/perf" build -p 8 -o "$go_build/perf_multi" ./multi
artifact -x "$go_build/perf_multi"

step mkdir -p "$py_native"
step cp -L "$runtime" "$py_native/libzlink.so.1.26.0"
step ln -s libzlink.so.1.26.0 "$py_native/libzlink.so.0"
step ln -s libzlink.so.0 "$py_native/libzlink.so"
step "${build[@]}" bash -c 'cd "$1" && exec /root/toolchains/venv/bin/python3 setup.py build_ext --inplace --force -j 8' _ "$stage/bindings/python"
artifact -f "$py_native/libzlink.so.1.26.0"
extension_suffix=$(/root/toolchains/venv/bin/python3 -B -c 'from sysconfig import get_config_var; print(get_config_var("EXT_SUFFIX"))')
artifact -f "$stage/bindings/python/src/zlink/_native/_zlink_native$extension_suffix"

step "${build[@]}" cmake -S "$stage/bindings/cpp" -B "$cpp_build" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LTO=OFF "-DZLINK_CORE_DIR=$core" "-DZLINK_CPP_CORE_BUILD_DIR=$core" \
    -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON -DZLINK_CPP_BUILD_BENCHMARKS=ON
step "${build[@]}" cmake --build "$cpp_build" --parallel 8 --target "${cpp_targets[@]}"
artifact -f "$cpp_build/CMakeCache.txt"
for target in "${cpp_targets[@]}"; do artifact -x "$stage/bindings/cpp/perf/multi/build/$target"; done

step "${build[@]}" dotnet build "$project/Zlink.BindingBench.Multi.csproj" -c Release -m:1
artifact -f "$project/bin/Release/net8.0/Zlink.BindingBench.Multi.dll"

run_group() {
    local lang=$1 round=$2 pattern=$3 run_sizes=$4 label name out
    local -a env_options=() options=() measure=()
    label=${languages[lang]}
    name="$id-${slugs[lang]}-r$round-$pattern"
    out="/root/perf-runs/$name"
    # Every runner takes the C option names since #6054 (Rust, Go, Python) and #6069.
    case "$label" in
        Rust) env_options=("${rust_env[@]}") ;;
        Go)
            env_options=("${go_env[@]}")
            options=(--build-dir "$go_build")
            ;;
        Python) env_options=("${python_env[@]}") ;;
    esac
    options+=(--latency-duration 5 --monitor-hwm-bytes 4096000)
    # C forwards --io-threads to run_comparison.py, which rejects it; C already
    # defaults to 4 server and 4 client I/O threads (C baseline run.log).
    if [[ $label != C ]]; then options+=(--io-threads 4); fi

    [[ ! -e "$out" && ! -L "$out" ]] || fail "Result directory already exists: $out"
    if (( execute )); then
        preflight || exit 75
        mkdir "$out"
    else
        command_line preflight
        command_line mkdir "$out"
    fi
    measure=("${common_env[@]}" "${env_options[@]}" "$run_one" "$name" \
        "$self" --execute --locked-runner "$stage/${runners[lang]}" \
        --reuse-build --pattern "$pattern" --transports tcp --msg-sizes "$run_sizes" \
        --runs 1 --duration 5 --clients 100 --part-count 2 \
        --connect-concurrency 128 --connect-ready-timeout-ms 10000 --server-ready-timeout-ms 10000 \
        --server-shutdown-timeout-ms 5000 --server-bind-port 0 --auto-hwm-profile balanced \
        --transport-transition-ms 3000 --pattern-transition-ms 3000 --sndtimeo 200 --rcvtimeo 200 \
        "${options[@]}")
    if (( execute )); then
        step "${measure[@]}" 2>&1 | tee "$out/console.log"
    else
        command_line "${measure[@]}"
        printf '  console capture: %s\n' "$out/console.log"
    fi
    step "$table" expect "$out" "$pattern" tcp "$run_sizes"
    if (( execute )); then printf '%s\t%s\n' "$label" "$out" >> "$index"; fi
}

for lang in "${!languages[@]}"; do
    for pattern in "${patterns[@]}"; do run_group "$lang" 1 "$pattern" "$sizes"; done
    selection=$unstable
    if (( lang > 0 )); then
        repeat_cmd=("$table" repeat --index "$index" --language "${languages[lang]}")
        command_line "${repeat_cmd[@]}"
        if (( execute )); then
            additional=$("${repeat_cmd[@]}")
            selection+=$'\n'"$additional"
        else
            printf '# Extend the following size lists with out-of-range cells from the repeat command.\n'
        fi
    fi
    for round in 2 3; do
        for pattern in "${patterns[@]}"; do
            repeat_sizes=$(awk -F '\t' -v p="$pattern" '$1 == p {print $3}' <<< "$selection" | sort -nu | paste -sd, -)
            if [[ -n $repeat_sizes ]]; then run_group "$lang" "$round" "$pattern" "$repeat_sizes"; fi
        done
    done
done

command_line "$table" compare --index "$index"
printf '  table output: %s\n' "$campaign/table.md"
if (( execute )); then "$table" compare --index "$index" > "$campaign/table.md"; fi
