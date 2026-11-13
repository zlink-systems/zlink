#!/usr/bin/env python3
import json,pathlib,re,subprocess
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868'); raw=job/'raw'
s=json.loads((raw/'summary.json').read_text())
manifest=json.loads((raw/'build-provenance.json').read_text())
passed=sum(c['L0']['pass_0_95'] for c in s['matrix'])
if passed<6:
    assert (raw/'cost-findings.md').exists(),'Below-threshold cells require attributed profile/count evidence'
    cost=(raw/'cost-findings.md').read_text()
else:
    cost=('기준 미달 셀이 없어 추가 profile은 실행하지 않았다. 이것이 L0의 추가 비용이 0이라는 뜻은 아니다. '
          'L0에도 고정 envelope bytes와 BenchPayload의 protobuf encode/decode와 native message 생성·복사, 벤치 stamp 검증·counter, '
          '실제 owner의 poll/receive와 binding의 native admission·completion이 남는다. '
          '각 비용의 독립 수치는 이번 처리량 비교에서 산출하지 않았다. 위치는 '
          '[encoder](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp:117), '
          '[binding send](/root/worktree/zlink-v21-s033/l3868/bindings/cpp/src/Runtime/Messaging/send_operations.cpp:30), '
          '[binding poller](/root/worktree/zlink-v21-s033/l3868/bindings/cpp/src/Runtime/Eventing/poller.cpp:360)에 있다.')
repo=pathlib.Path(manifest['worktree'])
tables=(raw/'tables.md').read_text()
# Include all seven restoration groups and every recorded disabled call site.
inventory=(raw/'disabled-code-table-owner.md').read_text()
text=f'''# #4350 C++ L0 결과

L0÷동시 측정 binding의 0.95 기준은 **{passed}/6셀 {'통과' if passed==6 else '통과 — 미달 셀 분석은 아래 기재'}**다. 각 중앙값과 원본 경로는 [집계](raw/summary.json)에 있다. 실험 코드는 로컬 브랜치 `exp/3868-cpp-ladder`의 `{manifest['commit']}`에만 커밋했다. push·PR·merge는 하지 않았다.

PM 2026-11-12 08:30 UTC 결정과 09:00 UTC 응답 작업 정정을 적용했다. 이번 세션에서는 L0와 binding만 측정했다. main(L8)은 [r4190w 보고서](../r4190w/report.md)의 `d3fd955131` 참조값이며 재측정하지 않았다. 앞선 `control-*` raw는 보존하고 성능 판정에서 제외했다. `inherited-main-sync.txt`는 소스 대조 자료이며 main 측정 자료가 아니다.

조건은 kairos 측정 서버, source CPU 0–7 / target CPU 8–15, warmup 30초 / active 5초 / 3회 중앙값이다. 모든 실행은 r4190w의 `run-group.sh`를 JOB/raw 경로만 바꾼 사본의 exclusive measure lock 안에서 수행했다. [실행 로그](raw/l0-corrected-driver.log), [lock 시작](raw/lock-windows/l0-corrected-before.txt), [lock 종료](raw/lock-windows/l0-corrected-after.txt), 각 셀의 `conditions-*.txt`가 근거다.

빌드는 이 worktree의 Release preset, `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`, 지정 Core prefix `/root/.cache/zlink/core-release/1.25.0/linux-x64`, `build-l3868`, `cpp-build-slot.sh` / `-j3`를 사용했다. [빌드](raw/build-l0-owner-final.log), [검증](raw/verify-build-owner.txt), [빌드 출처·binary SHA-256](raw/build-provenance.json), [셀 직전 기록](progress.md)을 보존했다. compiler 입력의 Framework·binding·bench 소스와 생성 protobuf는 모두 이 worktree에 속하며 `/root/zlink` 바이너리는 사용하지 않았다. third-party vcpkg cache와 지정 native Core prefix는 재사용했다. 각 셀의 `binary-provenance.json`은 실행 직전 다시 확인한 commit·dirty flag·base·binary 경로·SHA-256을 담는다.

기준선 r4190w raw의 Core/binding metadata는 1.24.0이고, 이번 지정 Core/binding은 1.25.0이다. main 표는 PM 지정 역사 참조이며 버전이 같은 새 control로 해석하지 않는다. [기준선 metadata](../r4190w/raw/bench/cpp/run1/zlink-cpp-send-saturation-1024/results.json), [이번 metadata](raw/L0-owner/send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json).

{tables}

각 msg/s 링크는 그 중앙값과 일치하는 raw 실행을 가리킨다. 전체 반복별 수치·정산·원시 경로는 [summary.json](raw/summary.json), 원래 aggregator의 G5 판정은 [L0 집계](raw/aggregates/L0.json)에 있다. main send-saturation 4K는 원래 binding의 G5 편차 초과로 unsupported였다. 표의 main 비율은 수치 참고이며 이를 유효한 G5 판정으로 바꾸지 않았다. 이번 L0의 0.95 판정은 동시 측정 binding 분모만 사용한다.

교차 시험은 L0 send-saturation 1K만 수행했다. raw/raw와 Framework/Framework는 같은 세션의 기본 matrix raw를 그대로 재사용했고, Framework server/raw client만 별도 실행했다. raw client는 기존 `zlink_raw_driver_t`이며 새 Framework 모사 client가 아니다. target 비율은 `(FW server/raw client)÷(raw/raw)`, source 비율은 `(FW/FW)÷(FW server/raw client)`다. 두 비율은 합산 손실률이 아니다. 교차 시험의 [세 실행 집계](raw/summary.json)와 [별도 실행 로그](raw/l0-corrected-driver.log)를 보존했다.

구성 대안은 기존 full call/dispatch 각 단계에 개별 분기를 두는 방식과 실제 owner 내부에 직접 송수신 진입점을 두는 방식이었다. 후자를 사용해 L0의 queue·completion 경로를 binding과 직접 대조하도록 했다.

실제 실행 경로는 `raw_mesh_node_owner_t::start`로 구성한 socket·poller에서 `ablation_send` / `ablation_request` / `ablation_receive`를 호출한다. 수신 owner는 readiness에서 대기하고 EAGAIN까지 drain한 뒤 고정 handler 하나를 직접 호출한다. request 수신은 binding과 같은 `ParseFromArray`를 사용한다. 응답은 새 envelope Message, 새 BenchPayload body 복사, protobuf encode, 새 body Message를 만든 뒤 `received.reply()`에 2 parts로 제출한다. 요청 part는 응답에 재사용하지 않는다. 매 셀 직전 코드 발췌와 작업 일치는 `progress.md` 및 각 셀 `reply-parity.md`에 남겼다. 완료는 binding의 `admitted` / `reply`를 기존 bench의 `observe_binding`으로 관찰한다. `route_client_t`·`mesh_node_runtime_t`·`route_handler_invoker_t`·provider completion dispatcher를 실행한 것으로 표기하지 않는다.

벤치 전용 대체물은 `fixed_handler_t`의 단일 handler 호출, static envelope와 binding protobuf 호출, optional raw-mesh peer의 admission/liveness·canonical wire adapter다. 이들은 실제 Framework 기능의 완전한 구현이 아니다. 기본 L0 matrix와 교차 시험에는 raw-mesh canonical adapter를 사용하지 않았다. bytes는 기존 raw bench와 호환하며 wire round-trip·손상 입력 거부 검사는 [검증 로그](raw/verify-build-owner.txt)에 있다.

복원 위치는 다음과 같다. 정상 runtime은 원래 파일에 유지했고, 실험 진입점에서 기능을 제외했다. L1–L4의 encoder/view 경계, L5의 owner/queue 경계, L6의 handler 활성화 경계, L7의 completion 경계를 구분했다. 이 작업은 L0만 구현하며 level 1–7은 실행을 거부한다. 따라서 이후 단계는 아래 원래 소유자를 복원한 뒤 각각 검증해야 한다. 기능을 모두 한꺼번에 복구한 결과를 중간 단계로 측정하지 않는다.

{inventory}

소유 계층: 물리 route 선택·admission·HWM 재시도·native completion은 binding/Core다. L0는 이를 다시 구현하지 않는다. 제외한 Framework 기능의 소유 기준은 [Application job queue §3]({repo}/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:82), [Messaging hot path §3·§4]({repo}/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:66), [Submit과 completion]({repo}/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:1)이다.

교차언어 대조 범위: 사용자가 지정한 C++ 단독 L0 실험이며 타 언어 runtime과 public spec은 변경하지 않았다. 변경 분류는 C(사용자가 승인한 실험 전용 기능 우회)다. 제품 runtime의 A/B 수정·계약 준수를 주장하지 않는다. 제품 병합 대상이 아니다. 상속한 topology 변경은 실험 block을 빼면 당시 `origin/main` 스냅샷과 같음을 [소스 대조](raw/inherited-main-sync.txt)로 확인했다.

수정 전/후 물리 admission·재시도 판정 위치 수는 1→1(binding/Core)이다. Framework에 두 번째 물리 수락 상태·retry queue·lock을 추가하지 않았다. L0의 binding protobuf 호출과 고정 handler는 L4 Framework codec·L6 activation의 실험 대체물이며 제품 상태가 아니다.

남은 비용: {cost}

검증 결과는 [bench tests](raw/bench-tests-owner.txt), [정상·비정상 result 검사](raw/checker-self-test-current.txt)에 있다. 모든 판정 대상 실행은 errors=0, submitted=completed=received, abandoned=0, header validation failures=0, drain bound 미도달을 확인했다. 실패·불완전 실행이 발생했다면 보존 raw와 제외 이유를 아래에 기재한다.

변경 파일은 C++ bench의 CMake/preset·client/server·`ladder/`, Framework의 `detail/ablation_ladder.hpp`·`raw_mesh_node_owner.*`·상속한 `service_topology_registry.*`다. 정확한 목록은 [로컬 commit](raw/final-git.txt)에 있다. 인접 호출 경로 점검에서 별도 제품 수정으로 넘긴 항목은 없으며, 제외한 기능은 모두 위 복원 목록에 기록했다. C++ build directory는 작업 종료 시 제거하고 raw는 보존했다.
'''
(job/'report.md').write_text(text)
print(f'report: {passed}/6 threshold cells')
