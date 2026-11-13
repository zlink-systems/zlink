from pathlib import Path
import json
J=Path('/root/jobs/fx/cx/v21/s033/l3868-l0f'); R=Path('/root/worktree/zlink-v21-s033/l3868')
s=json.loads((J/'raw/summary.json').read_text()); a=json.loads((J/'raw/final-audit.json').read_text()); f=json.loads((J/'raw/target-frame-difference.json').read_text())
def rel(p): return str(Path(p).relative_to(J))
def rate(st): return f"[{st['median_msg_s']:,.0f}]({rel(st['median_raw'])})"
def source(label,path,line): return f'[{label}]({R/path}:{line})'
lines=['# #4350 C++ L0 receive-parity 측정','',
'측정은 완료했다. send-saturation 1K의 중앙값 비율은 [0.9337](raw/aggregates/send-saturation-1024.json)로 미달이다. send-saturation 4K와 request-serial 4K는 비율 기준을 넘었지만 [G5가 실패](raw/final-audit.json)했다.','',
'Branch `exp/3868-cpp-ladder`, commit [`73285401ca`](raw/build-provenance.json), clean. 이번 작업에서 제품 소스 수정·추가 커밋·push·PR은 하지 않았다. [최종 상태·정리](raw/cleanup.json).','',
'Release·bindings source ON·Core prefix [1.25.0](raw/CMakeCache.txt) 빌드를 사용했다. [변경 source 대비 바이너리 최신성](raw/changed-source-freshness.json), [전체 출처·SHA-256](raw/build-provenance.json), [빌드 로그](raw/build.log)를 보존했다.','',
'한 exclusive lock 세션에서 source CPU [0–7](raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json), target CPU [8–15](raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json), warmup [30초](raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json), active [5초](raw/L0/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json)로 측정했다. [lock 시작·종료](raw/measurement-completion.json), [시간 한도 확인](raw/final-audit.json).','',
'각 실행 전 commit·dirty·base·바이너리 경로와 SHA-256, binding/L0 수신·응답 코드 및 record당 wait·allocation·atomic·copy 대조를 [progress.md](progress.md)에 기록했다. 수신은 같은 blocking recv이며 응답은 새 envelope·새 protobuf body를 생성한다. 모든 실행에서 오류가 없고 submitted=completed=received를 확인했다. [전체 결과 검사](raw/final-audit.json).','',
'중앙값 단위는 msg/s다. 비율은 L0÷binding이며 기준은 [0.95](raw/summary.json)다. G5 spread는 `max(|run−median|)÷median`의 백분율이다. G5 실패 셀은 원래 실행을 포함해 추가 측정했고, request-serial 4K는 처음부터 [5회](raw/aggregates/request-serial-4096.json) 측정했다.','',
'| 셀 | 횟수 | L0 중앙값 | binding 중앙값 | 비율 | spread L0 / binding | 판정 |',
'|---|---:|---:|---:|---:|---:|---|']
for c in s['matrix']:
 p=c['pattern'];size=c['payload_size'];agg=f'raw/aggregates/{p}-{size}.json'; l=c['l0'];b=c['binding']
 judgement='미달' if not c['pass_0_95'] else ('G5 실패' if not c['g5_pass'] else '통과')
 def spread(st): return f"{st['g5_spread_percent']:.2f}%"+(' 실패' if st['g5']=='fail' else '')
 lines.append(f"| {p} {size//1024}K | [{len(l['runs'])}]({agg}) | {rate(l)} | {rate(b)} | [{c['ratio']:.4f}]({agg}) | [{spread(l)} / {spread(b)}]({agg}) | {judgement} |")
lines+=['','교차 시험은 send-saturation 1K이며 표의 순서는 server / client다.','',
'| 교차 조합 | 횟수 | 중앙값 msg/s | G5 spread · 판정 |','|---|---:|---:|---|']
for combo,label in [('raw-raw','raw / raw'),('framework-raw','L0 / raw'),('framework-framework','L0 / L0')]:
 st=s['cross'][combo];agg=f'raw/aggregates/cross-{combo}.json'
 lines.append(f"| {label} | [{len(st['runs'])}]({agg}) | {rate(st)} | [{st['g5_spread_percent']:.2f}% · {st['g5']}]({agg}) |")
lines+=['',f"Server 교체 비율은 [{s['target_retention']:.4f}](raw/summary.json), client 교체 비율은 [{s['source_retention']:.4f}](raw/summary.json)다. 교차 시험의 L0/L0÷raw/raw는 [{a['cross_overall_ratio']:.4f}](raw/final-audit.json)로, 기본 셀의 감소가 이 교차 시험에는 나타나지 않았다.",'',
'미달 셀인 send-saturation 1K에서 raw·L0 target의 paired perf를 수집했다. [raw perf.data](raw/profiles/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/capture/perf.data), [L0 perf.data](raw/profiles/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/capture/perf.data). 아래는 target PID로 필터링한 self CPU-clock 비중이다.','',
'| target frame | raw | L0 | 소스 위치 |','|---|---:|---:|---|']
raw=f['raw']['target_self_raw']; l0=f['l0']['target_self_raw']
lines += [
 f"| 공통 `recv_whole_envelope` | [3.47%]({raw}) | [2.98%]({l0}) | {source('native_receive.hpp:177','bindings/cpp/src/Runtime/Native/native_receive.hpp',177)} |",
 f"| receive loop | `command_loop` [0.08%]({raw}) | `ablation_receive` [0.08%]({l0}) | {source('raw:84','framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp',84)}, {source('L0:531','framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp',531)} |",
 f"| handler callback | `command_loop` 본문에서 처리 | [0.16%]({l0}) | {source('ladder_wire.hpp:307','framework/bench/grpc/cpp/ladder/ladder_wire.hpp',307)} |",
 f"| `fixed_handler_t::handle` | 별도 frame 없음 | [0.70%]({l0}) | {source('ladder_wire.hpp:209','framework/bench/grpc/cpp/ladder/ladder_wire.hpp',209)} |",'',
 f"Raw는 `command_loop → router.recv`이며 L0는 `direct_server → ablation_receive → router.recv`, 처리 시 `callback → fixed_handler_t::handle`이다. [raw stack]({f['raw']['target_stacks_raw']}), [L0 stack]({f['l0']['target_stacks_raw']}), [frame 추출 원본](raw/target-frame-difference.json). L0 handler에는 part·legacy view 검사와 protobuf decode·공통 metrics 호출이 있다. Frame 비중은 추가 연산량의 직접 계수가 아니다.",'',
 f"진단 쌍의 throughput 비율은 [{a['profile_ratio']:.4f}](raw/final-audit.json)였다. 교차 시험과 이 쌍에서는 기본 셀의 기준 미달이 반복되지 않았으므로, 기본 셀의 감소 전체를 위 target handler 비용으로 귀속할 수 없다. 진단 실행은 matrix 중앙값에 합산하지 않았다.",'',
'검증: [built unit tests](raw/bench-tests.txt) 전부 통과. [wire 양성·음성 검사, build dry-run, 실험 블록 밖 source 동일성 검사](raw/verify-build.txt) 통과. [최종 raw·median·spread 재계산 검사](raw/final-audit.json) 통과.','',
'산출물은 이 보고서·[progress.md](progress.md)·[tools](tools/)·[raw](raw/)다. 이전 도구에서 JOB/raw 경로를 바꾸고, 이번 brief에 맞춰 종료 시각과 최초 추가 반복 대상만 조정했다. [실행 제어 diff](raw/tools-controls.diff), [run-group·preflight 경로 변경 검사](raw/wrapper-validation.txt). C++ build directory와 이 작업의 bench process는 [정리 완료](raw/cleanup.json)했으며 raw는 모두 보존했다.','',
'남은 실패는 send-saturation 1K 비율 미달과 표에 표시한 G5 실패다. 감소 원인의 정량 귀속은 확정하지 못했다.']
(J/'report.md').write_text('\n'.join(lines)+'\n')
print(J/'report.md')
