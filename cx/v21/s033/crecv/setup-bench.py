from pathlib import Path
j=Path(__file__).parent
old=Path('/root/jobs/fx/cx/v21/s033/csubmit')
(j/'bench').mkdir(exist_ok=True)
s=(old/'bench/targets.cmake').read_text()
# The regular four endpoints keep the exact csubmit instrumentation. Exclude unrelated PM targets.
s=s[:s.index('file(READ "${CMAKE_CURRENT_BINARY_DIR}/bench_framework_server.cpp" _pm)')]
s+='\ntarget_sources(bench_cpp_zlink_server PRIVATE "${_bench_job}/roles.cpp")\ntarget_link_options(bench_cpp_zlink_server PRIVATE "LINKER:--wrap=zlink_router_recv")\n'
(j/'bench/targets.cmake').write_text(s)
(j/'bench/defer.cmake').write_text('cmake_language(DEFER CALL include "'+str(j/'bench/targets.cmake')+'")\n')
for name in ['run-B.sh','runner-common.sh','collect.py','census.sh','census-common.sh']:
 s=(old/name).read_text().replace(str(old),str(j))
 (j/name).write_text(s)
# Keep all pre-existing helper paths except the collector, whose snapshots go under our OUTPUT_DIR.
# collect.py references no production sources and enforces the memory floor itself.
