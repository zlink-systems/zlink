# Core REQUEST WRITABLE 조사 결과

상태: **BLOCKED — correlation work/count 부족 시 대기 토큰 계약의 spec gap.** 공개 C API에서 spurious WRITABLE을 재현하고 원인을 확정했다. Runtime 수정은 하지 않았다. 재현·회귀 테스트를 추가했으며, 해당 결함을 검출하는 64 KiB assertion은 실패 상태로 남긴다. Integration 125/126, 전체 175/176 통과. 두 게이트의 유일한 실패는 신규 재현 테스트다. 종료 코드 2는 spec gap에 따른 구현 보류를 뜻한다.

## 재현 결과

- 작업 위치: `/home/hep7hep7/project/zlink-wt-core-writable`, detached HEAD `769f44fa37`. Branch 전환 및 commit/push/reset/checkout/stash 없음. 기존 `core/build`, `core/build-dev` symlink 미사용.
- 원본 `doc/plan/c016-worklog/evidence/repro_request_writable.c`는 설명과 달리 **inproc**를 사용한다. 원본은 변경하지 않았으며 새 테스트가 inproc와 TCP를 모두 검증한다.
- 공개 C API DEALER → ROUTER. 64 B / 4 KiB / 64 KiB body와 빈 FINAL로 구성한 multipart REQUEST. ROUTER가 첫 request의 두 part를 받은 뒤 reply를 보류한다. 경쟁 sender 없음. 전체 request를 DONTWAIT로 재제출하고 completion queue를 NO_DATA까지 비운다.
- 새 테스트 1회당 조건별 5개 표본, 총 5회 실행으로 아래 각 행 25개 표본. 원본의 200 ms reply timeout 대신 5,000 ms를 사용하며, 측정 루프에는 200 ms 관측 상한을 둔다. 이는 측정 중 timeout에 의한 실제 credit 반환을 배제하기 위한 테스트 조건이다. Runtime timeout/budget 변경은 없다.

| Transport | Body | BP 횟수 / WRITABLE 횟수 | Reply 전 성공 | Reply 후 최종 성공 | 측정 시간 min / median / max (ms) |
|---|---:|---:|---:|---:|---:|
| inproc | 64 B | 0 / 0 | 25/25 | 25/25 | 0.002 / 0.003 / 0.009 |
| inproc | 4 KiB | 0 / 0 | 25/25 | 25/25 | 0.002 / 0.003 / 0.009 |
| inproc | 64 KiB | 매번 1,000 / 1,000 | 0/25 | 25/25 | 5.559 / 7.842 / 9.135 |
| TCP | 64 B | 0 / 0 | 25/25 | 25/25 | 0.014 / 0.017 / 0.031 |
| TCP | 4 KiB | 0 / 0 | 25/25 | 25/25 | 0.014 / 0.015 / 0.020 |
| TCP | 64 KiB | 매번 1,000 / 1,000 | 0/25 | 25/25 | 7.193 / 8.605 / 9.220 |

64 KiB WRITABLE 간격(µs): inproc 표본별 p50 범위 3.090–3.900, p95 범위 11.678–15.409, 전체 min/max 2.155/247.952. TCP 표본별 p50 범위 3.565–3.816, p95 범위 14.645–15.825, 전체 min/max 2.058/268.241. 각 표본의 첫 간격은 관측 시작부터 첫 WRITABLE까지이며 이후는 직전 WRITABLE부터의 간격이다. 시간에는 payload 초기화, public API 호출, queue drain과 assertion 비용이 포함된다.

재현 로그: `/home/hep7hep7/project/zlink-work/c016/core-writable-repro-{1,2,3,4,5}.log`.

## 원인과 spec 대조

1. `core/doc/spec/core/systems/06-auto-hwm.ko.md:492`의 pending request 수용은 physical HWM과 별도의 **32 MiB logical work budget / count 16,384**를 둔다. Accounted size가 32 KiB를 넘으면 charge는 `32 MiB + 1`이며, 빈 pair에서 한 건만 허용한다(:499–504). 따라서 64 KiB REQUEST가 unresolved인 동안 두 번째 REQUEST 거절 자체는 계약에 맞는다. 메시지 수 16개로 환산한 auto-HWM 가설이 이 재현의 원인은 아니다.
2. `core/src/api/socket/socket_request_reply_submit_api.cpp:108`의 observer가 `pipe_t::try_reserve_request_correlation()`을 호출한다. `core/src/runtime/core/pipe.cpp:1595`에서 work 부족을 판정하고 :1605–1608에서 `_request_correlation_waiting = true`, `ENOBUFS`로 거절한다.
3. `core/src/runtime/core/pipe.cpp:1699`는 원인을 `pipe_message_admission_request_full`로 보존하지만 :1707에서 errno를 일반 `EAGAIN`으로 바꾼다. DEALER의 `core/src/runtime/sockets/internal/lb.cpp:325`와 :332는 request-full 때문에 ordinary pipe를 비활성화하지 않는다. 이는 같은 pipe의 ordinary send를 막지 말라는 spec :514와 맞는다.
4. `core/src/api/socket/socket_request_reply_submit_api.cpp:240`–247은 `EAGAIN` 원인 구분 없이 SEND 공용 WRITABLE wait를 등록한다. 여기에서 correlation 거절이 physical-credit 대기 토큰으로 합쳐진다.
5. `core/src/runtime/sockets/common/socket_send_complete.cpp:210`–216은 토큰 등록 직후 readiness가 true이면 곧바로 WRITABLE을 발행한다. :89–94의 기본 predicate는 `xhas_out()`, DEALER는 `core/src/runtime/sockets/dealer/dealer.cpp:438`의 `_lb.has_out()`을 사용한다. 이 검사는 correlation work 부족을 보지 않는다. ROUTER predicate도 `core/src/runtime/sockets/router/router_send_path.cpp:603`에서 physical pipe readiness만 검사한다.
6. ROUTER가 첫 REQUEST를 dequeue하면 physical credit은 반환되지만 correlation work는 reply까지 남는다(spec :506–510). 따라서 **correlation-full / physical-writable**가 동시에 참이고, 재등록할 때마다 WRITABLE이 즉시 발행된다. 토큰 하나를 여러 번 발행한 문제가 아니라 **새 토큰마다 동일한 부적합 readiness를 재검사하는 문제**다.
7. 실제 correlation 회복은 `core/src/runtime/core/pipe.cpp:1617`의 `release_request_correlation()`과 :1634–1649의 owner mailbox activation으로 전달된다. 새 timer나 spin이 필요해서 발생한 문제가 아니다. Reply 후 재제출 성공도 이 경로와 일치한다.

소유 계층: **Core socket REQUEST admission / pipe correlation reservation / completion queue**. Binding 또는 Framework에서 보상할 문제가 아니다.
Spec 조항: socket README `:982`–997의 target credit·wake edge, `:1056`–1070의 REQUEST DONTWAIT 토큰·재제출 계약; auto-HWM `:490`–517의 독립 correlation work/count 및 회복 계약.
교차언어 대조: Framework runtime 변경 없음. .NET 담당자가 제공한 native repro와 같은 현상을 public C API에서 독립 재현했다. Node/Java/.NET 처리량 붕괴의 공통 원인 후보이지만 이 작업에서는 각 binding의 처리량을 재측정하지 않았으므로 전체 성능 원인으로 확정하지 않는다.
변경 분류: **D (토큰 계약 spec gap으로 runtime 구현 보류)**. 관측된 spurious signal 자체는 기존 Core 결함이며, 승인 후 수정 방향을 B로 확정할 수 있다.

## BLOCKERS — 필요한 계약 결정

`core/doc/spec/core/systems/06-auto-hwm.ko.md:512`–517은 correlation work/count 부족 시 flags·SNDTIMEO와 무관한 즉시 `BACKPRESSURED/EAGAIN`과 reservation 반환 시 request recovery를 규정한다. 하지만 **completion ID가 0인지 대기 토큰인지, 후속 WRITABLE 유무와 wake 조건은 규정하지 않는다**.

`core/doc/spec/core/socket/README.ko.md:1056`–1070은 HWM·byte credit·flow pause·target 미준비에 대해 토큰을 규정하고 SEND와 같은 wake edge를 사용한다. :995–997의 wake 목록에는 unresolved correlation 반환이 명시되어 있지 않다. Logical work charge는 allocator byte도 physical queue credit도 아니라는 auto-HWM :501–510의 구분 때문에 이 조항을 correlation-full에 자동 적용할 수 없다. Unified completion slot 포화는 :1050–1051에서 ID 0/no completion이라고 명시되어 있어, 그것을 별도 pair work/count 포화에 임의로 확대 적용하지 않는다.

선택지:

- **ID 0으로 즉시 실패:** correlation-full을 physical EAGAIN과 구분해 public API까지 전달하고 `BACKPRESSURED/EAGAIN, ID 0, completion 없음`으로 종료한다. 기존 REQUEST completion이 correlation 반환을 알리며 caller의 재시도 계기는 그 계약에서 정해야 한다. 추가 wait 상태는 적지만 caller의 토큰 기반 재시도 가정이 바뀐다.
- **Correlation 회복을 기다리는 토큰:** REQUEST work/count 대기임을 유지하고 해당 capacity 회복이 있는 경우에만 WRITABLE을 발행한다. Ordinary SEND는 physical credit 판정을 계속 사용한다. DEALER candidate 집합, 서로 다른 크기의 request, token 등록과 회복의 경합, 일부 work 반환만으로 wake가 가능한지 또는 요청 크기만큼 충분해야 하는지를 계약으로 정해야 한다. 현재 payload-free 토큰의 보존 정보(토큰·target·context)와의 관계도 필요하다.

일반 `check_write_admission()`에 correlation-full을 합치는 변경은 같은 pipe의 ordinary send를 막으므로 spec :514 위반이다. 토큰 등록 후 readiness 재검사만 제거하는 변경은 이미 지나간 wake를 잃는 경합을 만들 수 있다. 두 변경 모두 채택하지 않았다. Spec 수정 권한은 요청 범위에 없으므로 해당 문서는 변경하지 않았다.

## 변경·회귀 테스트

- `core/tests/integration/test_request_writable_credit.cpp`: public C API 재현을 통합 테스트로 작성. 전체 multipart 재제출, 토큰·context·WRITABLE result 검증, completion queue drain, reply 전 반복 상한, reply 후 두 REQUEST completion 성공을 검증한다. 각 메시지 크기와 transport를 5회 반복하고 분포를 출력한다. Reply 전 진행 없이 WRITABLE이 1회를 넘어 반복되면 실패한다.
- `core/tests/CMakeLists.txt`: 신규 테스트 등록, `LABELS "integration;serial"`, `RUN_SERIAL TRUE`, `TIMEOUT 30`.
- `core/src/**` 변경 없음. `socket_send_*` hot path 변경 없음. Spec/framework/bindings/doc/plan 변경 없음.
- 수정 전/후 runtime 규칙 수: **변경 없음**. 새 runtime 상태·timer·retry 규칙 없음.

## 게이트

- Configure: `cmake -S core -B core/build-writable -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_LTO=OFF -DZLINK_BUILD_TESTS=ON -DBUILD_TESTS=ON` 성공. `scripts/build-core.sh dev`와 같은 옵션.
- Build: `cmake --build core/build-writable -j3` 성공; 신규 target도 `-j3`로 빌드 성공.
- 신규 테스트 5회: 매회 6 cases 중 4 pass / 2 fail. Fail은 inproc/TCP 64 KiB의 spurious WRITABLE 상한 assertion으로 동일하며, 각 run에서 reply 후 최종 성공까지 검증했다. CTest exit 8.
- Integration `ctest --test-dir core/build-writable -L integration -j2 --output-on-failure`: **125/126 통과**, 214.63초, exit 8. 유일한 실패는 신규 `test_request_writable_credit`이며 기존 integration 125개는 통과. 로그 `core-writable-integration.log`.
- 전체 `ctest --test-dir core/build-writable -j2 -E hotpath_gate --output-on-failure`: **175/176 통과**, 249.15초, exit 8. 유일한 실패는 신규 `test_request_writable_credit`이며 기존 테스트 175개는 통과. 로그 `core-writable-full.log`.
- `git diff --check`: 최종 통과. Untracked 신규 소스도 `git diff --no-index --check /dev/null core/tests/integration/test_request_writable_credit.cpp`로 검사해 통과.
- Release+LTO hotpath_gate: 요청대로 실행하지 않음.

## 영향 범위와 A용 한 줄

테스트에서 확정한 범위는 단일 DEALER→ROUTER pair의 DONTWAIT multipart 64 KiB REQUEST다. 4 KiB와 64 B의 두 번째 REQUEST는 성공했다. 64 KiB의 pair당 unresolved REQUEST 한 건 제한은 현재 spec의 정책이므로, spurious WRITABLE을 수정하더라도 이 admission 한도는 유지된다. Code상 correlation work/count가 부족한 다른 크기·depth, ROUTER request도 같은 공용 토큰 경로의 영향을 받을 가능성이 있다. 그 조건들은 이번 재현으로 검증한 범위에 포함하지 않는다.

A용 한 줄: **Core 64 KiB REQUEST는 독립 correlation work budget에서 막히지만 공용 wait 등록이 physical pipe readiness로 즉시 WRITABLE을 발행해 TCP/inproc 각 25/25 표본에서 1,000회 busy loop 재현; correlation-full의 ID 0 vs 전용 회복 토큰 계약이 spec gap이므로 runtime 무수정, 공개 API 회귀 테스트 추가, integration 125/126·전체 175/176 통과(실패는 신규 repro뿐).**

EXIT:2
