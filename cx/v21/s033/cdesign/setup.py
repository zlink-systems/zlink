from pathlib import Path
import shutil
j=Path('/root/jobs/fx/cx/v21/s033/cdesign'); w=Path('/root/worktree/zlink-3867x'); a=Path('/root/worktree/zlink-3867a')
# Immutable copies of the existing #3888 executables; never build in either reference tree.
(j/'baseline-bin').mkdir(exist_ok=True)
for name in ['bench_cpp_client','bench_cpp_grpc_server','bench_cpp_zlink_server','bench_cpp_framework_server']:
 shutil.copy2(a/'framework/bench/grpc/cpp/build'/name,j/'baseline-bin'/name)
(j/'runners').mkdir(exist_ok=True)
runner=(w/'framework/bench/grpc/cpp/run_local.sh').read_text()
for variant,build in [('A',j/'baseline-bin'),('B',j/'build')]:
 text=runner.replace('source "${HERE}/../runner_common.sh"',f'source "{w}/framework/bench/grpc/runner_common.sh"').replace('BUILD_DIR="${HERE}/build"',f'BUILD_DIR="{build}"')
 (j/'runners'/f'{variant}.sh').write_text(text)
f=w/'framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp'; s=f.read_text()
old='''    dispatcher->post ([work = std::move (work), owner, dispatcher] {
        const completion_turn_scope_t scope{owner != nullptr ? owner : &process_completion_owner,
                                            dispatcher};
        work ();
    });'''
new='''    // EXPERIMENT ONLY: deliberately discard the fresh-turn and ingress separation boundary.
    const completion_turn_scope_t scope{owner != nullptr ? owner : &process_completion_owner,
                                        dispatcher};
    work ();'''
assert old in s;s=s.replace(old,new);f.write_text(s)
f=w/'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp';s=f.read_text()
old='''                  if (!executor->try_submit_internal ([start] { start (false); }))
                      start (true);'''
assert s.count(old)==1;s=s.replace(old,'''                  // EXPERIMENT ONLY: execute the existing owner drain on the receive resource.
                  start (false);''');f.write_text(s)
