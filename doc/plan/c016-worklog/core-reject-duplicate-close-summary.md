# Core REJECT 중복 pipe 종료 검증

2026-09-05. 감독 검토용 **부분 구현 / BLOCKED** 기록이다. 요청한 전체 완료 조건을 충족하지
못했다. 중복 pipe 종료는 구현했지만, 거부된 pipe의 REQUEST를 `NOT_CONNECTED`로 종결하는
부분과 기존 inproc 즉시 재연결 회귀가 남아 있다. 현재 diff는 commit하면 안 된다.

- 작업 트리: `/home/hep7/project/zlink-core-a`, detached HEAD
  `fadfac1c4e02c33d1a8eb19f39457e14e5a61b7d`.
- 변경: 아래 Core 파일만 수정했다. 이 요약만 main 트리에 생성했다. Commit하지 않았다.
- `bindings/`, `framework/`, spec 원본과 main의 `core/build-dev`, `zlink-core-b`는 수정하지 않았다.
- 증거 디렉터리: `/home/hep7/project/zlink-core-a/core/build-dev/reject-validation/`.
  아래 log 파일명은 이 디렉터리를 기준으로 한다.

소유 계층: Core ROUTER가 RID admission을 결정하고, Core pipe/session이 종료·재연결을,
기존 request/reply pending owner가 REQUEST terminal을 소유한다.

Spec 조항: `core/doc/spec/core/socket/README.ko.md:159`의 §4 REJECT 계약,
같은 파일 `:1134`의 §6 completion 원인 표, `07-router.ko.md` §2·§3의 receive/reply 계약.

교차언어 대조: 동일 candidate 공유 라이브러리로 C++ 계약 16개·sample 7개와 Python
테스트 190개·subtest 4개·sample 7개를 검증했다. Framework runtime 변경은 없으며,
이 결과가 binding에서 D-088의 해결을 입증하는 것은 아니다.

변경 분류: 구현한 close는 **A(개정 계약 적응)**. REQUEST terminal을 연결하려면 거부 사유
전달 계약을 정해야 하므로 **D(아래 BLOCKERS)**가 남는다. 기존 inproc 사례의 candidate 회귀도
미해결이다.

## 원인과 변경 파일

| 파일:line | 원인 또는 변경 |
|---|---|
| `core/src/runtime/sockets/router/router_admission.cpp:361` | HEAD의 REJECT 분기는 `false`만 반환했다. 기존 RID owner는 유지되지만 새 pipe는 route를 얻지 못한 채 연결을 유지했다. Candidate는 lifetime ref를 확보해 기존 `route_adoption_actions_t::terminate_pipe`에 거부된 pipe를 넘긴다. |
| `core/src/runtime/sockets/router/router_admission.cpp:468` | 기존 `finish_route_adoption()`이 route·transport lock 밖에서 `terminate(true)`와 ref 해제를 처리한다. 이 경로를 재사용했으며 별도 pending 상태나 index를 추가하지 않았다. |
| `core/src/runtime/sockets/router/router_recv_path.cpp:135` | 거부된 pipe의 anonymous lifecycle tracking은 유지된다. `_out_pipes`에 중복 RID를 등록하지 않으며 기존 pipe도 교체하지 않는다. 이 파일은 변경하지 않았다. |
| `core/tests/integration/test_router_reject_duplicate.cpp:167` | 공개 C API로 기존 RID 소유자의 request 성공, 중복 connector의 Disconnected, 기존 pipe 종료 뒤 자동 재연결·request 성공을 TCP와 inproc에서 검사한다. |
| `core/tests/integration/test_router_reject_duplicate.cpp:210` | D-088의 첫 request 성공 → client disconnect → 같은 connection의 Disconnected → connect → READY edge → 한 번의 request submit 순서를 재현한다. 100ms 안에 server 수신 또는 NOT_CONNECTED completion을 요구한다. |
| `core/tests/CMakeLists.txt:123` | `test_router_reject_duplicate`를 integration CTest에 등록했다. |

Admission 수정은 `identify_peer()`가 호출하는 `adopt_peer_routing_id()`에 있다.
종료 전달은 `session_base.cpp:453`의 기존 `pipe_peer_terminated()`와
`socket_base_api.cpp:1721`의 기존 종료 경로를 따른다.

## 테스트와 gate 결과

빌드 명령:

```bash
cd /home/hep7/project/zlink-core-a
nice -n 10 env JOBS=4 scripts/build-core.sh dev
```

빌드는 성공했다. `RelWithDebInfo`, LTO OFF, JOBS=4다. CTest는 모두 `-j2`로 실행했다.
새 테스트는 sleep, 내부 symbol, failpoint를 사용하지 않는다. 동일 socket에 poller 하나만
등록하며, 등록한 poller로 reply/completion 진행을 계속 처리한다.

| 검증 | 결과 |
|---|---|
| `test_reject_duplicate_close_tcp` ×20 | **20/20 PASS**. 기존 owner의 통신 유지, duplicate 종료, 기존 owner 종료 뒤 duplicate의 자동 재연결과 request 성공. |
| `test_reject_duplicate_close_inproc` ×20 | **20/20 PASS**. 동일 조건. |
| `test_reject_disconnect_reconnect_tcp` ×20 | **0/20 PASS, 20/20 FAIL**. READY 뒤 성공한 submit에 대해 100ms 안에 수신·terminal이 없다. |
| `test_reject_disconnect_reconnect_inproc` ×20 | **20/20 PASS**. 재접속 pipe가 수용되고 request/reply 성공. 거부된 inproc REQUEST의 exactly-once terminal 검증을 통과했다고 주장하지 않는다. |
| 기존 ROUTER suite | **5/5 PASS**: `test_router_reciprocal_handover_lanes`, `test_router_handover`, `test_router_mandatory_hwm`, `test_router_multiple_dealers`, `test_router_mandatory`. 40.82초. |
| 전체 `ctest --test-dir core/build-dev --output-on-failure -j2` | **168/171 PASS, 3 FAIL**, 271.40초. 전체 suite는 한 번 실행했다. 실패 목록은 아래 BLOCKERS. |
| `^test_single_lane_` 1회 / 2회 | **29/29 PASS / 29/29 PASS**, 20.27초 / 20.30초. |
| C++ contract / sample-smoke | **16/16 PASS / 7/7 PASS**, 합계 23/23. |
| Python | 최초 188 PASS + subtest 4 PASS, 복사한 검증 트리의 참조 파일 누락으로 2 FAIL. 읽기용 참조 경로를 보완한 뒤 해당 2개만 재실행해 **2/2 PASS**. 합계 190개와 subtest 4개 검증 완료, sample **7/7 PASS**. |
| raw header mirror | c/cpp/go/rust × `zlink_enum.h`, `zlink/socket/api.h`, `zlink/eventing/api.h`: **12/12 일치**. |
| `git diff --check` | PASS. |

반복 결과: `zlink-core-a-reject-repeat-final.log`.
전체 결과: `zlink-core-a-reject-full-ctest.log`.
Single-lane: `zlink-core-a-reject-single-lane-{1,2}.log`.
Binding: `zlink-core-a-reject-cpp-gate.log`, `zlink-core-a-reject-python-gate.log`,
`zlink-core-a-reject-python-gate-fixed.log`.

Binding script가 원본 `bindings/`에 산출물을 쓰지 않도록 C++는 같은 CMake 옵션·CTest label로
`core/build-dev/cpp-gate`에서 실행했다. Python은 tracked 파일 150개를 byte 동일하게 복사한
`core/build-dev/reject-python-gate-repo`에서 native extension을 빌드하고 원본과 동일한
`tests/run_tests.sh -p no:cacheprovider`를 실행했다. 두 언어 모두
`/home/hep7/project/zlink-core-a/core/build-dev/lib/libzlink.so.0.17.0`을 사용했다.

## Hotpath 수치

`scripts/`에는 별도 hotpath 검사기가 없다. Valgrind가 설치되어 있어 전체 CTest에 등록된
`core/tests/perf/hotpath_gate.py`를 실행했다. Gate는 **4/4 cell FAIL**이며 green으로 세지 않는다.

원인 분리를 위해 현재 object·archive에서 admission object만 정확한 HEAD source로 교체한
baseline을 `core/build-dev/reject-baseline`에 만들었다. Candidate source나 reference 파일은
변경하지 않았다. 같은 compiler flag와 동일 benchmark object로 baseline을 링크해 측정했다.
이 dev baseline도 저장 기준을 초과한다. 아래 단위는 instructions/message이며 REQUEST cell은
instructions/request다.

| cell | 반복 수 | 저장 기준 | HEAD dev baseline | candidate dev | HEAD 대비 |
|---|---:|---:|---:|---:|---:|
| dealer_dealer_inproc | 20,000 | 3455.3810 | 4476.094250 | 4476.047150 | -0.001052% |
| dealer_router_reqrep_inproc | 5,000 | 12054.8948 | 15146.265200 | 15155.335000 | +0.059881% |
| pair_inproc | 20,000 | 2681.9566 | 3531.221800 | 3531.193100 | -0.000813% |
| router_router_tcp | 20,000 | 2972.8817 | 3855.916150 | 3855.903400 | -0.000331% |

Candidate의 저장 기준 대비 비율은 각각 1.2954, 1.2572, 1.3166, 1.2970이다.
측정 log는 `zlink-core-a-reject-hotpath-baseline.log`와 전체 CTest log에 있다.
Release/LTO gate를 실행하거나 reference를 갱신하지 않았다.

## BLOCKERS

1. **거부된 REQUEST의 terminal 미구현 — D-088 TCP 20/20 실패.**
   `socket_base_api.cpp:1716`의 `pipe_peer_terminated()`는 request terminal을 호출하지 않으며,
   `:1721`의 최종 pipe 종료도 pending REQUEST를 `NOT_CONNECTED`로 처리하지 않는다.
   `socket_request_reply_dispatch.cpp:455`의 기존
   `fail_pending_requests_for_transport_pair()`가 요구한 one-shot owner다. 현재 호출자는
   ROUTER handover뿐이다. 거부하는 ROUTER의 request state에 이 helper를 호출해도 TCP 반대편
   DEALER가 소유한 pending request는 처리되지 않는다.

   일반 종료 전체에 helper를 호출하는 대안은 socket README §6 `:1141`의
   **transient physical disconnect → replay 없이 기존 budget 유지** 계약을 바꾼다.
   기존 TCP close는 `asio_engine.cpp:1926`에서 transport disconnect로 관측되며 RID 거부 사유가
   전달되지 않는다. ZMP ERROR도 `asio_zmp_engine.cpp:705`에서 현재 `EPROTO`로 처리한다.
   명시적인 거부 사유를 전달하고 그 사건에만 기존 helper를 연결하는 대안에는 protocol/error
   의미 결정이 필요하다. 일반 transient disconnect를 일괄 NOT_CONNECTED로 바꾸거나 원격
   pending registry에 직접 접근하는 우회는 추가하지 않았다. 이 범위 결정은 감독 확인이 남아 있다.

   `zlink-core-a-reject-d088-tcp.log`에는 새 pipe의 `routing_id_ok=0`, terminate,
   `process_pipe_term_ack`, anonymous pipe 정리까지 기록되지만 REQUEST terminal은 없다.
   정확히 한 번의 NOT_CONNECTED/EHOSTUNREACH 및 timeout 뒤 duplicate-terminal 부재는
   검증 완료 조건으로 남는다.

2. **기존 inproc 즉시 reconnect 사례에서 WRITABLE이 오지 않는 candidate 회귀.**
   전체 gate의 `test_socket_disconnect_progress_without_app_poll`은
   `test_inproc_immediate_disconnect_connect_default_policy_observation`의
   `test_socket_disconnect_progress_without_app_poll.cpp:473`에서 실패하고, 실패 뒤 teardown이
   끝나지 않아 executable timeout 60초로 기록됐다. 이 사례는 client terminal을 기다리기 전에
   disconnect→connect하는 overlap이며, 새 D-088 inproc의 terminal 이후 reconnect와 다르다.

   원인 분리용 public case는 원본 assertion·본문을 그대로 두고 main에서 해당 사례만 실행했다.
   Admission object만 바꾼 HEAD baseline은 PASS, candidate는 같은 `:473`에서 FAIL이었다.
   관련 log는 `zlink-core-a-reject-baseline-comparison.log`다. 기존 pipe와 거부된 pipe의
   server-side term ack 처리가 teardown 때까지 지연되는 trace를 관찰했다.
   종료 시 drain 여부를 원인 후보로 비교한 `terminate(false)` 실험도 같은 실패여서
   그 실험 변경은 최종 source에 적용하지 않았다(`zlink-core-a-reject-no-drain-comparison.log`).
   Core command 진행·WRITABLE 회복 경계의 원인 수정이 필요하다. Fixture나 assertion은
   변경하지 않았다.

3. **Hotpath 저장 기준 gate 미통과.**
   같은 HEAD dev baseline부터 4개 cell이 저장 기준을 초과한다. Candidate의 HEAD 대비 최대
   증가는 0.059881%이지만, 이것을 release gate 통과로 대체하지 않는다.

최종 수정 파일은 worktree의 `router_admission.cpp`, `core/tests/CMakeLists.txt`, 새 public
contract test와 main의 이 요약이다. Runtime 변경은 uncommitted 상태다.
