from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/wire')
for label,tree in [('W','/root/worktree/zlink-3867w'),('main','/root/worktree/zlink-ratio040')]:
 runner=Path(tree+'/framework/bench/grpc/cpp/run_local.sh').read_text()
 runner=runner.replace('HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',f'HERE="{tree}/framework/bench/grpc/cpp"')
 runner=runner.replace('source "${HERE}/../runner_common.sh"',f'source "{job}/runner_common-{label}.sh"')
 # CPU pinning is already supplied by the requested patch on run_local.sh.
 runner=runner.replace('setsid taskset -c 8-15 "${target_command[@]}"','setsid taskset -c 8-15 env LD_PRELOAD=/root/jobs/fx/cx/v21/s033/cprof/alloc_counter.so PROF_ALLOC_DIR="${PROF_ALLOC_DIR}" "${target_command[@]}"')
 runner=runner.replace('setsid taskset -c 0-7 "${BUILD_DIR}/bench_cpp_client"','setsid taskset -c 0-7 env LD_PRELOAD=/root/jobs/fx/cx/v21/s033/cprof/alloc_counter.so PROF_ALLOC_DIR="${PROF_ALLOC_DIR}" "${BUILD_DIR}/bench_cpp_client"')
 (job/f'run-{label}.sh').write_text(runner)
 common=Path(tree+'/framework/bench/grpc/runner_common.sh').read_text().replace('BENCH_GRPC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',f'BENCH_GRPC_DIR="{tree}/framework/bench/grpc"')
 needle='  wait_for_idle "${source_url}"\n  [[ -s "${result_file}" ]]'
 common=common.replace(needle,f'  python3 {job}/collect.py "${{a_pid}}" "${{b_pid}}" "${{cell_dir}}/capture"\n'+needle)
 active='  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active'
 common=common.replace(active, '  ls "/proc/${a_pid}/task" >"${cell_dir}/source-preactive-tids.txt"\n'+active)
 assert f'{job}/collect.py' in common
 (job/f'runner_common-{label}.sh').write_text(common)
