**Core reciprocal HANDOVER 조사 결과 — 2026-09-05**

Core 결함을 공개 C API로 재현하고 수정했다. 원인은 inproc의 **connect-before-bind에서 connector pipe의 방향 flag가 보존되지 않는 것**이다. 양쪽 bind를 먼저 마친 동시 connect는 TCP/inproc 모두 정상이다. .NET B1은 첫 node가 상대의 bind 전에 connect하는 시작 순서 때문에 실패했다. Framework의 중복 connect나 동일 local RID 설정이 원인은 아니다.

작업 트리는 `/home/hep7/project/zlink-core-a`, 기준 HEAD는 `3975cea2556a6858c9b97b41b5c1942368a22934`다. 요청대로 detached 상태에서 작업했고 commit하지 않았다. Main에는 이 보고서만 작성했다.

- **소유 계층:** Core의 inproc pipe 생성과 ROUTER route admission.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md:145-167` §4 HANDOVER 및 `07-router.ko.md:153-155` §5 standby 유지. Superseded request의 즉시 `REQUEST_NOT_CONNECTED` 계약은 bb730c654f/8b82d51b75를 따른다.
- **교차언어 대조:** .NET `StatefulServiceRuntimeTests.cs:902-908`의 시작 순서를 공개 C API로 옮겨 같은 `0/0`과 `101`을 재현했다. 언어별 보상은 필요하지 않으며 C++/Python도 수정된 공통 Core로 회귀 검증했다. Framework runtime은 수정하지 않았다.
- **변경 분류:** **B — 기존 Core 결함 수정.** Public API와 계약 변경 없음.

**공개 C API 재현**

두 ROUTER의 RID는 `A`와 `Z`, duplicate policy는 HANDOVER다. Alias를 쓰는 경우 각 connect에 상대 RID를 `CONNECT_ROUTING_ID`로 설정했다. TCP는 loopback의 동적 port, inproc은 같은 context를 사용했다.

| 수정 전 조건 | 결과 |
|---|---|
| 양쪽 bind 후 두 thread가 동시에 connect, TCP, alias 없음/있음 | 40/40 정상. 교체 trace는 반대 방향 `0/1` 또는 `1/0`. |
| 같은 동시 connect, inproc, alias 없음/있음 | 40/40 정상. 반대 방향으로 판단. |
| A가 Z의 bind 전에 connect, inproc, alias 없음/있음 | 두 case 모두 첫 반복에서 `existing_local=0 new_local=0`, reply completion `REQUEST_TIMED_OUT(101)` 재현. |
| Z가 A의 bind 전에 connect, inproc, alias 없음/있음 | 40/40 reply 성공이지만 `0/0` 오분류는 매번 발생. 이 순서는 우연히 같은 survivor를 선택한다. |
| 기존 D-B96 순차 `test_reciprocal_handover_tcp_100ms` | 1/1 정상. |

실패를 결정적으로 만드는 공개 호출 순서는 다음과 같다. Timeout이나 sleep으로 경합을 유도하지 않는다.

```c
zlink_bind(A, endpoint_a);
zlink_connect(A, endpoint_z);  /* Z는 아직 bind하지 않음 */
zlink_bind(Z, endpoint_z);
zlink_connect(Z, endpoint_a);
/* 양쪽 readiness 이후 REQUEST → receive → reply → completion */
```

수정 전 A 선행 case는 `Expected 0 Was 101`에서 실패했다. 실패 후 fixture 강제 종료도 멈춰 외부 `timeout 20`이 프로세스를 종료했다. 이 종료 제한은 재현 실행에만 사용했고 contract test의 timeout과 assertion은 완화하지 않았다.

**원인과 .NET trace 대조**

`local`은 해당 socket이 시작한 connect 쪽이라는 뜻이다. READY lane의 종류나 peer RID에서 정하는 값이 아니다.

1. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:330` 부근의 inproc pipe 생성에는 `set_locally_initiated()`가 없었다. TCP connector는 같은 파일의 현재 `:681`, session에서 생성하는 pipe는 `core/src/runtime/core/session_base.cpp:534`에서 이미 방향을 저장한다.
2. Connect-before-bind Application intent는 `socket_base_api.cpp:291-309`에서 admission을 보류한다. 첫 `attach_pipe(..., locally_initiated=true)`의 인자는 이 반환 뒤 보존되지 않는다.
3. 상대 bind 시 `ctx_inproc_registry.cpp:382-384`가 connector에도 bind command를 보낸다. `socket_base_lifecycle.cpp:1331`은 기본 인자로 `attach_pipe(pipe)`를 호출한다. `socket_base_api.cpp:465-467`의 `locally_initiated_ || application->is_locally_initiated()`는 둘 다 false가 된다.
4. 따라서 A에서 기존 A→Z outbound와 새 Z→A inbound가 모두 `local=0`이다. `router_admission.cpp:314-315`의 same-direction 분기가 RID 비교보다 먼저 실행돼 Z→A로 교체한다. Z는 기존 A→Z를 유지하므로 양쪽 survivor가 어긋난다.

보존한 `.NET` 원본 `dotnet-bilateral-strace.log:87-116`에서도 같은 순서가 보인다.

- 최초 A→Z pair `8507187213167924228/1`: A의 pipe `0x6fdb60028a80`가 `local=0`, Z의 pipe `0x6fdb6002a640`도 `local=0`.
- 역방향 Z→A pair `11404002622356820154/1`: Z의 pipe `0x6fdb60064e40`는 `local=1`, A의 pipe `0x6fdb600651b0`는 `local=0`.
- A는 `existing_local=0 new_local=0`으로 교체한다. 이어지는 `:128-133`의 send trace에서 Z는 최초 pair, A는 역방향 pair를 선택한다.

Framework trace에는 서로 다른 RID `bilateral-request-aa`/`bilateral-request-zz`와 각 node의 connect intent 1개가 기록돼 있다. `ZLinkManagedMeshNode.cs:308,339`는 node 시작 시 bind 후 보관한 connect intent를 실행한다. Fixture의 `caller.Start(); owner.Start();`가 위 connect-before-bind 순서를 만든다.

**Diff**

| 작업 트리 변경 파일 | 변경 |
|---|---|
| `core/src/runtime/sockets/common/socket_base_endpoint.cpp:331-333` | inproc connector pipe 생성 직후 기존 `set_locally_initiated(true)`를 호출. 실행 코드 1줄과 이유 주석 2줄. |
| `core/tests/integration/test_router_reciprocal_handover_lanes.cpp:553` | 기존 공개 API 테스트에 8개 case 추가, 각 20회 반복. |

방향을 ROUTER에서 endpoint/READY로 다시 추론하는 방안과, 생성자가 기존 pipe flag를 보존하는 방안을 비교했다. 후자를 선택해 transport 생성 지점에서 연결 방향을 한 번만 기록한다. 추가 상태, retry, poller, Framework 보상 또는 public API는 도입하지 않았다. Message hot path에는 코드를 추가하지 않았다.

RID 비교와 standby 유지, reply pair 제한, `router_admission.cpp:451-458`의 superseded pending request 종결은 기존 구현을 그대로 사용한다. 수정 후 A 선행은 A→Z를 유지하고, Z 선행은 `0/1`·`1/0` 비교로 A→Z를 선택한다.

**Contract test와 검증 결과**

추가한 case는 다음과 같다. 각 case는 20회 실행한다.

- `test_simultaneous_reciprocal_tcp`, `test_simultaneous_reciprocal_tcp_alias`
- `test_simultaneous_reciprocal_inproc`, `test_simultaneous_reciprocal_inproc_alias`
- `test_reciprocal_inproc_connect_before_bind_a_first`, `test_reciprocal_inproc_connect_before_bind_a_first_alias`
- `test_reciprocal_inproc_connect_before_bind_z_first`, `test_reciprocal_inproc_connect_before_bind_z_first_alias`

새 테스트는 초기 양방향 REQUEST/reply, 최대 한 번의 `NOT_CONNECTED` 후 재제출, 양쪽의 두 Application connection readiness 관측 후 양방향 survivor REQUEST/reply를 검사한다. Arbitration 동안 `DISCONNECTED`/`CLOSED`는 0이어야 한다. 기존 순차 case는 superseded request의 즉시 종결과 standby 두 lane의 유지·명시 해제를 계속 검사한다.

| 검증 | 결과 |
|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, RelWithDebInfo, LTO OFF, JOBS=4. |
| `ctest --test-dir core/build-dev -R '^test_router_reciprocal_handover_lanes$' --repeat until-fail:5 -j2` | **5/5 PASS**. 매회 Unity 16/16. 새 시나리오 총 **800/800**, 기존 시나리오 40/40. |
| 동시 출발 시간 차이, 위 5회 | TCP 200건 최대 **253µs**, inproc 200건 최대 **201µs**. 모두 1ms 미만. |
| 새 시나리오의 `NOT_CONNECTED` | 800건 모두 0회. 재제출 없이 첫 reply와 survivor reply 성공. |
| 기존 superseded request 검사 | reciprocal 최대 **20ms**, same-direction 최대 **22ms**. 기존 200ms assertion 통과, 중복 terminal 없음. |
| 전체 `ulimit -v 16777216; ctest --test-dir core/build-dev -j2 --output-on-failure` | **145/146 PASS**, 219.16초. 유일한 실패는 아래 `hotpath_gate`. Wake-invariant 포함 기능 테스트는 모두 PASS. |
| `ctest --test-dir core/build-dev -R '^test_single_lane_' -j2` ×2 | **29/29 + 29/29 PASS**. |
| C++ contract / samples | **16/16 + 7/7 PASS**. `bindings/cpp`는 읽기만 하고 별도 build 디렉터리에 구성했다. `ldd`로 작업 트리의 `core/build-dev/lib/libzlink.so.0` 사용 확인. |
| Python test / samples | **190 tests + 4 subtests / 7/7 samples PASS**. 원본에 native extension이 없어 검증용 복사본에서 빌드했다. 복사본에 필요한 spec 읽기 경로를 제공한 뒤 전체 통과. 원본 `bindings/**` 변경 없음. |
| C/C++/Go/Rust 공개 header mirror | **12/12 동일** (`zlink_enum.h`, `zlink/socket/api.h`, `zlink/eventing/api.h`). |
| `git diff --check` | PASS. |

**BLOCKERS**

`hotpath_gate`는 dev 구성에서 기준 대비 25.7~31.7% 초과로 실패했다. 수정 전 HEAD의 `socket_base_endpoint.cpp`를 **동일한 dev compiler flags**로 별도 컴파일하고, static archive 복사본의 해당 object만 교체한 control binary로 원인을 분리했다. 수정 전도 네 cell 모두 같은 수준으로 실패한다.

| Cell | 수정 전 HEAD instr/msg | 수정 후 instr/msg | 수정 전후 차이 |
|---|---:|---:|---:|
| dealer_dealer_inproc | 4476.05755 | 4476.03925 | -0.0004% |
| dealer_router_reqrep_inproc | 15150.29120 | 15157.48080 | +0.0475% |
| pair_inproc | 3531.21140 | 3531.25320 | +0.0012% |
| router_router_tcp | 3855.87715 | 3856.09415 | +0.0056% |

이번 수정으로 발생한 hotpath 회귀는 관측되지 않았지만 **전체 gate를 green으로 판정하지 않는다**. Reference는 변경하지 않았고 release/LTO gate는 실행하지 않았다. Dev 구성과 기준값의 차이에 대한 최종 판정은 감독자에게 남긴다.

원래 .NET Stateful 4개와 Canonical 1개의 end-to-end 재실행은 이 Core job에서 하지 않았다. 감독자는 Core 변경과 local package 갱신 후 해당 5개 테스트를 재검증해야 한다. 이 보고서는 그 5개가 이미 green이라고 주장하지 않는다.

전체 log 36개는 `/home/hep7/project/zlink-core-a/core/build-dev/reciprocal-evidence/`에 보존했다. 주요 근거는 `baseline-test_reciprocal_inproc_connect_before_bind_a_first*.log`, `dotnet-bilateral-strace.log`, `dotnet-bilateral-framework.log`, `reciprocal-repeat5-last-trace.log`, `core-full-ctest.log`, `hotpath-baseline-gate.log`, `single-lane-{1,2}.log`, `cpp-{contract,samples}.log`, `python-native-tests-final.log`다. 검증용 복사본과 임시 build 산출물은 제거했고, 수정된 Core library는 작업 트리의 `core/build-dev/lib/`에 유지했다.
