# XPUB/dist attach-terminate symmetry — fix (M6A runtime heap corruption)

Owner-layer fix for the intermittent glibc `double free or corruption (out)` /
ASan `heap-buffer-overflow` diagnosed in
`fix-cpp-m6a-runtime-heap-corruption-summary.md`. Public C-API repro:
`pub_churn.cpp` (SUB connect/close churn against a live PUB under publish load).

## 원인 (cause, file:line)

한 사실("pipe가 dist에 attach되어 있는가")을 attach와 terminate가 다르게 판단하는
**비대칭**이 원인이다.

1. `socket_base_t::attach_pipe` — `core/src/runtime/sockets/common/socket_base_api.cpp:272-273`
   ```cpp
   if (pipe_->has_completed_termination ())
       return;                    // dist_t::attach() 이전 조기 return
   ```
   PUB이 attach 명령을 처리하기 전에 peer(SUB)가 pipe를 이미 종료시키면 `dist_t::attach()`가
   건너뛰어지고, 해당 pipe의 `array_item_t::_array_index`는 기본값 `-1`로 남는다.
   (같은 조기 return을 통과하더라도 실제 `xattach_pipe`는 몇 줄 뒤 `is_lifecycle_active()`로
   한 번 더 게이트되므로, 비-paired 소켓에서 attach는 조건부다.)

2. `socket_base_t::pipe_terminated` — `core/src/runtime/sockets/common/socket_base_api.cpp:1877-1883`
   PUB은 비-paired(pair_id==0)라 `application_attached`가 항상 참 → 위 pipe에 대해서도
   무조건 `xpub_t::xpipe_terminated(pipe_)` 호출.

3. `xpub_t::xpipe_terminated`(`xpub.cpp:312`) → `dist_t::pipe_terminated`(`dist.cpp:96`) →
   `_array_index == -1`이므로 세 카운터(_matching/_active/_eligible) 감소가 모두 skip되고,
   `array_t::erase((size_type)-1)`(`utils/array.hpp:89`)가 버퍼 base-8byte에 8byte WRITE →
   heap-buffer-overflow. 이후 임의 free에서 `double free or corruption (out)`로 표면화.
   (덤프의 `_pipes length 0, _active 1, _eligible 1` 불일치는, 다른 SUB 하나는 정상 attach된
   상태에서 never-attached SUB의 erase(-1)가 벡터를 pop_back하여 정확히 재현되는 상태다.)

`dist_t::has_pipe()`(`dist.cpp:43`) 멤버십 확인이 이미 존재했으나 이 경로에서 사용되지 않았다.

## 수정 (fix)

**컨테이너 소유의 단일 불변식**: "dist에 attach된 pipe만 dist에서 terminate한다." 이미 같은
규칙을 적용하는 `fq_t::pipe_terminated`(멤버십 없으면 조기 return)와 통일한다.

- `core/src/runtime/sockets/internal/dist.cpp` — `dist_t::pipe_terminated` 진입부에
  `if (!has_pipe (pipe_)) return;` 추가. dist_t가 자기 배열 멤버십의 단일 소유자이므로 이
  한 곳으로 **XPUB과 XSUB 두 사용처가 모두** 보호된다(호출부 개별 가드 대신 소스에서 수정).
- `core/src/runtime/sockets/internal/lb.cpp` — `lb_t::pipe_terminated`도 동일 클래스의 잠재
  결함(비멤버에 대한 `_pipes.erase` → erase(-1))을 갖고 있어 `if (!_pipes.contains (pipe_)) return;`
  추가. (`lb_t::attach`는 `_pipes`와 `_entries`를 항상 함께 채우므로 가드가 정상 cleanup을
  건너뛰는 경우는 없다.) DEALER/ROUTER는 paired 경로의 `application_attached`가 attach와
  대칭이라 실제 크래시 경로는 아니었으나, 세 배열 컨테이너(fq/dist/lb)가 하나의 규칙을
  공유하도록 맞춘다.

`socket_base_api.cpp`의 조기 return은 "죽은 pipe를 attach하지 않는다"는 올바른 정책이므로
유지하고, terminate 쪽을 그 정책과 대칭이 되도록 컨테이너에서 맞췄다.

검사 대상 컨테이너: dist_t(수정), lb_t(수정), fq_t(이미 가드 존재). radio/dish 계열 소켓은
현재 코드베이스에 없음(`core/src/runtime/sockets/` = common/dealer/internal/pair/proxy/pubsub/
router/stream). ROUTER out-pipe(`_out_pipes`/`_standby_pipes`)는 array_t가 아닌 std::map 기반
이라 동일 OOB 클래스에 해당하지 않음.

## 완료 보고 4줄

- **소유 계층**: Core — 배열 기반 pipe 컨테이너(dist_t/lb_t/fq_t)의 terminate 회계. socket 계층
  attach/terminate 대칭. Framework 아님.
- **spec 조항**: connection teardown(pipe 종료) 결정은 Core 소유
  (AGENTS.md §3 "계층 소유권", `core/doc/spec/core/socket/` PUB/XPUB). Framework/binding 무관.
- **교차언어 대조**: 네이티브 Core 결함 → 언어 무관. 동일 Core를 쓰는 모든 binding의
  PUB/XPUB(및 XSUB) 소켓이 "연결 즉시 종료가 attach와 race"하는 동일 조건에서 영향받았다.
- **변경 분류**: **B (기존 결함, Core)**. spec/fixture/binding 변경 없음.

## 규칙 수 (before / after)

- **before: 2** — "pipe가 dist에 붙는가"를 attach(조기 return + lifecycle 게이트)와
  terminate(pair_id==0 → application_attached 무조건 참)가 서로 다르게 판단(비대칭).
- **after: 1** — "dist에 attach된 pipe만 dist에서 terminate한다"는 단일 멤버십 불변식.
  특수 분기·잔존 카운터 제거. 또한 세 배열 컨테이너(fq/dist/lb)가 동일 규칙을 공유하여
  코드베이스 전체 규칙 수도 감소(이전엔 fq만 가드, dist/lb는 예외).

## 검증 결과 (results)

- **재현 증명(unfixed)**: dist 가드를 임시 제거한 ASan 빌드에서 `test_pubsub_churn_dist`가
  CPU 부하 하 4회째에 abort — 진단서와 **동일 스택**
  (`array_t::erase` @ `utils/array.hpp:89` ← `array.hpp:72` ← `dist_t::pipe_terminated`,
  `WRITE of size 8`, `heap-buffer-overflow`).
- **신규 테스트**: `core/tests/integration/test_pubsub_churn_dist.cpp` (공개 C API 전용:
  PUB/XPUB에 대한 SUB connect+즉시 close churn 500×3, 백그라운드 DONTWAIT publish 부하).
  `core/tests/CMakeLists.txt`의 `tests` 목록 + regression/serial/network-lock/TIMEOUT 180으로 등록.
- **ASan(수정본)**: `core/build-asan`(`ENABLE_ASAN=ON`, RelWithDebInfo)에서 CPU 부하 하
  `test_pubsub_churn_dist` 12/12 통과 **ASan 0 errors**. 기존 pub/sub 테스트
  (`test_pubsub`, `test_pubsub_filter_xpub`, `test_xpub_nodrop`,
  `test_pubsub_close_during_inbound_frame`) 모두 **ASan 0 errors**.
- **부하 반복(normal dev build)**: `ctest -R '^test_pubsub_churn_dist$' --repeat until-fail:10`
  CPU 부하 하 **10/10 통과**.
- **타겟 스위트**: `ctest -R 'test_pubsub|test_xpub|test_sub|test_dist|test_transport_matrix|test_socket_disconnect_boundary'`
  → **7/7 통과**.
- **전체**: `ctest --test-dir core/build-dev -j2 -E '^hotpath_gate$'` → **181/181 통과 (0 실패)**.
  (1차 실행에서 `test_phase3_request_reply_contract`가 부하 경합으로 1회 Timeout →
  단독 재실행 11.92s green, CONTRIBUTING §4 load-flake 판정. 무부하 재실행에서 181/181 clean.)
- **hotpath_gate**: dev 빌드에서 미등록/미실행(§5, valgrind 필요) — green으로 세지 않음.

## BLOCKERS / 후속

- 없음. 성능 경로(dist distribute hot path)는 변경하지 않았고, 추가된 것은 종료 경로의
  멤버십 조기 return 한 줄이므로 hotpath 비용 영향 없음.
- 진단용 임시 빌드 `core/build-asan`은 남겨둠(원본 `core/build-dev`와 무관, 필요 시 삭제 가능).
- 패치: `core-xpub-dist.patch` (dist.cpp/lb.cpp/CMakeLists.txt/test 4파일, +139 lines).
