# Go request WRITABLE 테스트의 route-prime 실패 진단

## 판정

**Core의 기존 결함(B)이다.** 첫 DATA는 송신 admission에 실패하며, ROUTER가 DATA를
받은 뒤 버리는 문제가 아니다. Inproc routing-id preamble을 이미 읽은 peer의 byte credit을
pair hold 해제 경로가 확인하지 않고, credit 대기도 등록하지 않는다. 따라서 빈 pipe가
비활성 상태로 남고, 첫 SEND의 WRITABLE 토큰도 진행하지 않는다.

양쪽 `CONNECTION_READY`를 받은 뒤 첫 DATA를 보내는 공개 C API 재현에서도 5회 모두
실패했다. 기존 Go 테스트의 준비 단계가 현재 admission 계약에 어긋난다는 근거는 없다.
Go 테스트의 HWM·순서·assertion을 변경하지 않았다.

- 소유 계층: **Core `pipe_t`의 transport hold 해제와 byte-credit admission**.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md` §5 `:437–452`의 빈 pipe oversize
  admission·frame credit 반환, §6 `:986–1002`의 write-credit 회복·attach에 따른 WRITABLE.
  §4 `:160–168`의 RID admission과 READY 의미도 대조했다.
- 교차언어 대조: Go 공개 API와 binding을 사용하지 않는 공개 C API가 같은 실패를 보인다.
  Go `socket_routed.go:38–46,80–82`는 Core `NO_DATA`를 `(false, nil)`로 투영한다.
- 변경 분류: **B — 기존 Core 결함**. Go 계약 적응(A), 우회(C), spec gap(D)이 아니다.
- 수정 전/후 규칙 수: **구현 변경 없음**. 후속 Core 수정은 별도 cached-credit 판정과
  기존 credit 대기 규칙을 기존 owner의 규칙 하나로 통합하는 방향(제안: 2 → 1)이다.

## 원인과 관측 근거

원인 위치는 `core/src/runtime/core/pipe.cpp:1874–1882`의
`pipe_t::release_writes_for_transport_pair()`다. 직접 실패하는 판정은 `:1880`이다.

1. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:402–403`이 inproc
   routing-id preamble을 보낸다. `core/src/runtime/core/pipe.cpp:202–210,2113–2126`의
   preamble 쓰기는 일반 HWM 검사를 우회하지만 byte 회계에는 포함된다. 기본 RID 16 bytes와
   message metadata 64 bytes의 합은 80 bytes다.
2. `core/src/runtime/core/pipe.cpp:1867–1871`이 초기 pair hold를 설정하면서
   `_out_active=false`로 만든다. ROUTER는
   `core/src/runtime/sockets/router/router_admission.cpp:282–294`에서 preamble을 읽고
   RID를 등록한다. 로그의 `routing_id_ok=1`이 이를 확인한다.
3. `core/src/runtime/sockets/common/socket_base_api.cpp:554–557`이 hold를 해제한다.
   `pipe.cpp:1880`은 `check_hwm_unlocked()`로 sender의 cached credit만 확인한다.
   이때 `written=80`, `cached_read=0`, `HWM=1`이어서 full로 판단한다.
   `_transport_pair_write_held`는 false가 되지만 `_out_active`는 false로 남는다.
4. 이 경로는 이미 게시된 peer credit을 읽는
   `pipe.cpp:3378–3399`와 credit 대기를 등록하는 `:1778–1806`을 모두 사용하지 않는다.
   ROUTER의 `account_inbound_frame()`은 읽은 80 bytes를 `:3898–3903`에서 게시하지만,
   `:3956–3963`은 credit waiter가 있을 때만 `activate_write`를 보낸다.
   이 writer의 `_waiting_for_byte_credit`는 계속 false다.
5. Hold 해제가 false를 반환해 `socket_base_api.cpp:585–590`의 write activation도
   발생하지 않는다. SEND는 `BACKPRESSURED/EAGAIN`, nonzero token을 반환하고
   WRITABLE이 오지 않는다. ROUTER의 기본 1,000 ms recv timeout이 `NO_DATA/EAGAIN`을
   반환한다. 이 timeout 반환 자체는 socket README §6 `:538–542`와 일치한다.

`/tmp/zlink-go-prime/gdb-ready.log`의 관측값은 다음과 같다. 디버거는 breakpoint에서
필드를 읽었으며 Core 코드나 메모리 값을 변경하지 않았다.

| 관측 시점 | write hold | out active | written | cached peer read | peer published read | credit waiter |
|---|---:|---:|---:|---:|---:|---:|
| 양쪽 READY 이후 SEND 진입 | false | false | 80 | 0 | 80 | false |
| DATA recv·completion poll timeout 이후 | false | false | 80 | 0 | 80 | false |

실제 queue는 비어 있다. 실패하는 DATA payload는 11 bytes이며, single-part complete
message이므로 HWM 1에서도 빈 pipe oversize 규칙의 대상이다.
`core/doc/spec/core/systems/06-auto-hwm.ko.md:572`도 빈 queue가 complete message 한 건을
HWM 초과여도 수락하도록 명시한다. 공개 C 재현의 별도 대조군은
HWM 80에서 실패하고 81에서 통과했다. 이는 preamble 80 bytes의 cached charge와 실패
경계가 일치함을 보이는 진단 실험이며, HWM 상향을 해결책으로 제안한 것이 아니다.

### Admission 계약 대조

Socket README §4 `:166–167`은 connector READY가 상대 ROUTER의 RID admission까지
보장하지 않는다고 정한다. 이번 재현은 **ROUTER 자신의 READY와 DEALER의 READY를 모두
확인한 뒤** 보내며, ROUTER 로그에서도 RID 등록 성공을 확인했다. Peer가 하나뿐이므로
§4 REJECT/HANDOVER의 duplicate 판정에 진입하지 않는다.

D-092(`doc/plan/c016-worklog/decisions.ko.md:1247–1251`)는 preamble 때문에 종료 관측이
application recv에 의존하는 결함을 Core 소유로 판정했다. D-094(`:1281–1285`)는 같은
endpoint라는 이유로 REJECT를 우회하지 못하게 한다. 두 결정 모두 이번 첫 연결의
credit 회복을 막거나 READY 이후 빈 pipe의 DATA를 거절하는 근거가 아니다. 위반은
§6의 credit·WRITABLE 진행 계약이며, 추가 admission 정책이 필요한 상황도 아니다.

### 후속 Core 수정 방향

Cached credit만 최신화하면 preamble을 먼저 읽은 순서는 해결할 수 있지만, hold 해제
뒤에 preamble을 읽는 순서에는 credit waiter 등록이 필요하다. 기존
`pipe_t::hwm_credit_ready_unlocked()`(`pipe.cpp:1794–1806`)의 snapshot·waiter 등록·재확인
규칙을 hold 해제에도 재사용하는 대안을 권한다. 새 timer·retry·상태를 추가할 필요가 없다.
두 순서와 PAUSED·비활성 pipe의 기존 제약을 보존하는 Core 회귀 검증은 감독자 작업이다.
이번 작업에서는 Core를 수정하거나 수정 후 통과를 주장하지 않는다.

## 공개 재현

주 재현 소스는 `/tmp/zlink-go-prime/request-prime.c`다. 공개 C API만 사용하며 양쪽
READY 확인, 최초 SEND 한 번, DATA recv 한 번, 실패 토큰 관측으로 구성한다.
Sleep·재제출·timeout 증가가 없고, 실패 시 exit 1, 정상 admission과 정확한 DATA 수신 시
exit 0이다. Completion poll의 1,000 ms는 관측 상한이다.

```bash
cd /home/hep7/project/zlink
cc -std=c11 -Wall -Wextra -Werror -g -O0 -I core/include \
  /tmp/zlink-go-prime/request-prime.c -L core/build-dev/lib \
  -Wl,-rpath,/home/hep7/project/zlink/core/build-dev/lib -lzlink \
  -o /tmp/zlink-go-prime/request-prime
ZLINK_ROUTER_DEBUG=1 /tmp/zlink-go-prime/request-prime
```

현재 결과:

```text
router READY recv=0 count=1 flags=1
dealer READY recv=0 count=1 flags=1
hwm=1 SEND result=1 errno=11 id=1
DATA recv=201 errno=11 bytes=0
completion poll count=0 errno=0
completion recv=201 errno=11 kind=0 id=0
ready empty-pipe DATA regression: FAIL
```

기존 공개 Go 재현은 `/tmp/zlink-go-r3/request-prime-repro.go`를 그대로 실행했다.
기존 C 재현의 사본 `/tmp/zlink-go-prime/request-prime-original.c`도 현재 Core에 다시
링크해 같은 실패를 확인했다. 기존 진단 기능 `ZLINK_ROUTER_DEBUG`·
`ZLINK_DEBUG_PIPE_TERM`의 로그를 보존했고 임시 runtime 로깅은 추가하지 않았다.
Framework를 거치지 않으므로 Framework message-flow listener는 적용 대상이 아니다.

## 검증 결과

검증 시 HEAD는 `0989f20218cbdea8602ffe8f8437ec00cbe56fde`, branch는 `main`이었다.
`core/build-dev/lib/libzlink.so.0.17.0`과
`bindings/go/native/linux-x86_64/libzlink.so.0.17.0`의 SHA-256은 모두
`64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`다.
Core rebuild·package 갱신은 없었다.

아래 로그 경로의 기준 디렉터리는 `/tmp/zlink-go-prime/`다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 기존 공개 Go 재현 | 동일 `(false, nil)`·SEND 대기 | `go-public.log` |
| 기존 공개 C 재현, 현재 Core에 재링크 | BACKPRESSURED token 1, recv NO_DATA, WRITABLE 없음 | `c-original-trace.log` |
| 새 C 회귀 재현, HWM 1 | 실패 재현 5/5, 각 exit 1 | `c-ready-hwm1-run0.log` … `run4.log` |
| C 대조군 HWM 80 / 81 | 80 실패, 81 통과 | `c-ready-hwm80-run0.log`, `c-ready-hwm81-run0.log` |
| 기존 Go 대상 테스트, `-count=1 -v` | 하위 run-0…4 모두 prime에서 실패 | `go-targeted.log` |
| `flock /tmp/zlink-samples-gate.lock bash bindings/go/tests/run_tests.sh` | 실패 1개 테스트/하위 5건, 다른 패키지 통과 | `go-gate.log` |
| `go vet ./...` | 통과 | `go-vet.log` |
| script의 raw contract·hot-path guard 명령 | 통과 | `go-guards.log` |
| `flock /tmp/zlink-samples-gate.lock bash bindings/go/samples/run_samples.sh` | **7/7 통과** | `go-samples.log` |

전체 gate는 첫 `go test ./...` 실패로 중단하므로 vet·guard·samples는 별도로 실행했다.
같은 전체 gate를 반복하지 않았다. 디버거 스크립트와 원본 로그도 같은 `/tmp` 디렉터리에
보존했다(`trace-ready.gdb`, `gdb-ready.log`, `trace-send.gdb`, `gdb-send.log`).

## BLOCKERS

- **Core 수정 필요:** `pipe.cpp:1880`의 hold 해제·credit 회복 결함 때문에 Go 전체 gate의
  0 failures 조건을 충족하지 못한다. 공개 C 재현과 진단을 감독자에게 넘긴다.
- 기존 `TestPublicRequestRetriesExactPacketAfterWritable`은 prime에서 실패하므로
  이번 실행으로 실제 REQUEST의 exact-packet 재제출 단언까지 검증한 것은 아니다.
  Core 수정·Go local library 갱신 뒤 이 테스트와 전체 gate를 다시 검증해야 한다.
- 저장소 변경은 요청된 **이 보고서 하나**다. `bindings/go/**`, `core/**`, 보호된 spec,
  다른 작업자의 변경은 수정하지 않았다. Commit·push는 하지 않았다.
