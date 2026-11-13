# #4350 C++ L0 — 시간 한도 종료 · 미완료

소스 수정과 로컬 커밋은 완료했으나, 공용 측정 lock 대기로 빌드를 시작하지 못했다. 새 matrix·교차 시험은 [모두 미실행](raw/final-status.json)이며 L0/binding ≥0.95 여부와 잔여 비용의 정량 귀속은 판정할 수 없다. 이전 측정값을 이번 결과에 사용하지 않았다.

Branch `exp/3868-cpp-ladder`, commit `73285401ca18c376320a03e06350a4d9b1a60bb5`, working tree clean. [커밋·변경 파일](raw/commit.txt), [전체 patch](raw/source-change.patch). Push·PR·merge는 하지 않았다.

Raw request 수신 [bench_zlink_cpp_server.cpp:41](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:41)와 command 수신 [bench_zlink_cpp_server.cpp:84](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:84)에 맞춰 L0 [raw_mesh_node_owner.cpp:531](/root/worktree/zlink-v21-s033/l3868/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:531)가 같은 direct blocking recv loop를 사용하도록 바꿨다. 같은 stop.load와 recv 오류 처리이며, 별도 readiness wait·DONTWAIT drain을 제거했다. Receive timeout도 양쪽 모두 설정하지 않는다. [양쪽 수신·응답·source 비용 대조](raw/parity-rcvtimeo/reply-parity.md).

Source는 raw [bench_cpp_client.cpp:653](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/client/bench_cpp_client.cpp:653)와 L0 [ladder_driver.inc:115](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_driver.inc:115)가 공통 [bench_cpp_client.cpp:455](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/client/bench_cpp_client.cpp:455)를 호출한다. 기존 raw submit 본문을 그대로 옮겼으며 매 대조에서 원래 raw 본문과 일치하는지 검사한다. 빈 Message 초기화·move 대입·반환 array 이동을 실제 L0 source 경로에서 제거했다. 미사용 context·poller·per-stream encoder도 L0에서 생성하지 않는다. Source completion은 raw와 같은 POLLCOMPLETION wait다.

Native send buffer 복사는 공통 Core [encoder.hpp:92](/root/worktree/zlink-v21-s033/l3868/core/src/runtime/protocol/encoder.hpp:92)의 동작이며, L0만의 추가 복사로 분류하지 않는다. 이번 작업에서는 source가 같은 raw submit 구현을 사용하므로 Message 생성·복사·수명도 같은 코드가 결정한다. 새 perf profile이나 malloc/lock 실측은 수집하지 못했다.

응답은 raw [bench_zlink_cpp_server.cpp:58](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:58)와 L0 [ladder_wire.hpp:242](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp:242)의 새 envelope part·새 BenchPayload body 복사·protobuf encode·새 body part·2-part reply submit을 유지했다. 요청 part를 재사용하지 않는다.

| 변경 파일 | 결과 |
|---|---|
| [framework/bench/grpc/cpp/client/bench_cpp_client.cpp](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/client/bench_cpp_client.cpp) | raw submit을 공통 driver로 이동 |
| [framework/bench/grpc/cpp/ladder/ladder_driver.inc](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_driver.inc) | L0가 공통 submit·completion을 사용; 미사용 자원 생성 제거 |
| [framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp](/root/worktree/zlink-v21-s033/l3868/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp) | 직접 blocking recv; 미사용 submit helper 제거 |
| [raw_mesh_node_owner.hpp](/root/worktree/zlink-v21-s033/l3868/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp) | 실험용 poller 접근자; 미사용 선언 제거 |

| 셀 | L0 / binding 실행 | 비율 · 0.95 판정 |
|---|---|---|
| send-saturation 1K | [미실행](raw/final-status.json) | 판정 불가 |
| send-saturation 4K | [미실행](raw/final-status.json) | 판정 불가 |
| request-serial 1K | [미실행](raw/final-status.json) | 판정 불가 |
| request-serial 4K | [미실행](raw/final-status.json) | 판정 불가 |
| request-backpressure 1K | [미실행](raw/final-status.json) | 판정 불가 |
| request-backpressure 4K | [미실행](raw/final-status.json) | 판정 불가 |

| send-saturation 1K 교차 시험 | 결과 |
|---|---|
| raw / raw | [미실행](raw/final-status.json) |
| Framework L0 server / raw client | [미실행](raw/final-status.json) |
| Framework L0 server / Framework L0 client | [미실행](raw/final-status.json) |

통과한 검사는 [수신·응답·source 정적 대조](raw/parity-rcvtimeo/reply-parity.md), [실험 블록 밖 normal runtime 비교](raw/verify-normal-runtime.txt), [결과 검사기 양성·음성 검사](raw/checker-fixtures/validation.txt), [run-group 복사 범위 검사](raw/wrapper-validation.txt)다. 결과 검사기에는 역사 fixture를 사용했으며 성능 측정에 포함하지 않았다. Release build·wire 실행·bench 실행 검증은 미실행이다.

공용 gate는 측정 writer가 없어진 뒤에도 [기본 180초 grace](raw/measure-gate-keeper-observed.sh)를 요구한다. 측정이 계속 이어져 build-slot의 gate를 기다렸고, 마지막에는 build slot을 확보한 상태로 shared measure.lock을 기다렸다. [시간별 상태](progress.md), [종료 직전 lock](raw/final-lock-state.txt), [종료한 이 작업의 프로세스](raw/stopped-own-build.json).

[run-group.sh](tools/run-group.sh)는 이전 사본에서 JOB/raw 경로만 바꿨다. [빌드 명령](tools/configure-build-inner.sh)은 지정 build-slot·Release·bindings source ON·Core 1.25.0 prefix·-j3 조건으로 준비했다. [검증 후 측정 진입](tools/preflight-measure.sh), [전체 셀·교차·미달 profile 실행](tools/measure.sh)을 보존한다. 이 명령들이 실제 빌드·측정을 완료했다고 해석하지 않는다.

소유 계층: 물리 receive·admission·HWM retry·native completion은 Core/binding이다.

스펙 근거: [messaging hot path §1·§3·§4](/root/worktree/zlink-v21-s033/l3868/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md).

교차언어 대조: 사용자 지정 C++ 단독 L0 실험이다. 타 언어 runtime·public spec은 수정하지 않았다.

변경 분류: C(사용자가 승인한 실험 전용 기능 제외). 제품 A/B 수정·계약 준수를 주장하지 않는다.

규칙 수: 활성 source encode·submit 판정 위치 2→1. 물리 admission·retry 판정 위치는 1→1(Core/binding). 새 per-message 상태·queue·lock은 추가하지 않았다.

호출 경로 점검: 임시 Message 수명 작업, 미사용 source 자원·owner submit helper, target의 poller/DONTWAIT 수신을 제거했다. Native output buffer 복사는 양쪽 공통 Core 작업으로 확인했다. 범위 밖 제품 수정으로 넘긴 항목은 없다.

남은 검증은 새 Release build·wire/bench 실행 검증·전체 셀과 교차 시험·미달 셀의 profile 또는 allocation/lock count다. 현재 코드는 C++ 실행 검증을 거치지 않았다.

절차 이탈: 공통 규칙을 읽기 전에 기본 저장소 지침의 internal-docs.sh를 한 번 실행했다. 이후에는 실행하지 않았다.

종료 시 이 작업의 대기 프로세스와 Python cache를 정리했다. C++ build directory는 생성되지 않았다. [정리 결과](raw/cleanup.txt). 이전 l3868 raw와 이번 raw는 모두 보존했다.

기록 시각: 2026-11-13T10:17:30.845222+00:00.
