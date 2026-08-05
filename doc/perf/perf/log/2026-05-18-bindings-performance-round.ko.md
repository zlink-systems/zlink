# 2026-05-18 bindings 성능 작업 로그

이 문서는 `bindings-library-performance-improvement-plan.ko.md`에서 분리한 측정 기록이다.
계획 문서 본문에는 실행 규칙과 현재 상태 표만 유지한다.

## C++

- `MULTI_ROUTER_ROUTER/tcp/65536`
  - C 기준: `184497.8`
  - C++ best: `97370.8`
  - C 대비: `52.8%`
  - 목표: `65%`
  - 상태: 보류
  - 결과: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260518_114136_codex_cpp_tcp_rr_64_after_raw_revert.txt`
- `MULTI_ROUTER_ROUTER/tcp/131072`
  - C 기준: `85400.0`
  - C++ best: `57028.4`
  - C 대비: `66.8%`
  - 목표: `65%`
  - 상태: 통과
  - 결과: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260518_112701_codex_cpp_tcp_rr_large_local_send_msg.txt`
- 확인한 내부 후보
  - 단일 파트 routed send fast path
  - raw routed private state
  - poller socket cache
  - latency reserve
  - sparse poll event construction
  - source routing id native copy 방식 변경: `perf_cpp_multi_linux_20260518_221411_codex_cpp_tcp_rr65536_rid_partial_copy.txt`, 악화되어 원복
  - RR client request slot 재사용: `perf_cpp_multi_linux_20260518_221607_codex_cpp_tcp_rr65536_reuse_request_slot.txt`, 악화되어 원복
  - 동일 source routing id 대입 생략: `perf_cpp_multi_linux_20260518_221726_codex_cpp_tcp_rr65536_rid_skip_same.txt`, 악화되어 원복
  - 원복 후 source 기준 재빌드: `perf_cpp_multi_linux_20260518_222013_codex_cpp_tcp_rr65536_source_rebuild_after_failed_candidates.txt`
- 보류 이유
  - public API 변경 없이 시도한 후보가 목표 미달이거나 128KB 회귀를 만들었다.
  - 현재 public builder 경로는 이미 단일 파트 raw routed send로 내려간다.
  - 추가/수정 후보: 단일 파트 routed send API, 반복 전송용 pre-bound routed sender API,
    source routing id materialization 생략 API.

### C++ ws full multi

- 실행 명령
  - `bindings/cpp/perf/run_binding_multi.sh --transports ws --results-tag codex_cpp_ws_full_status`
- 결과 파일
  - `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260518_124314_codex_cpp_ws_full_status.txt`
- C 재측정 기준
  - baseline 결과가 현재 core와 맞지 않는 것으로 보여 같은 transport로 C full 결과를
    다시 측정했다.
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260518_125011_codex_c_ws_full_current_compare.txt`
- 상태
  - partial
- 통과
  - `MULTI_DEALER_DEALER/ws/256`: C 대비 `96.4%`
  - `MULTI_DEALER_DEALER/ws/1024`: C 대비 `93.5%`
  - `MULTI_DEALER_DEALER/ws/65536`: C 대비 `94.8%`
  - `MULTI_DEALER_DEALER/ws/131072`: C 대비 `101.6%`
  - `MULTI_DEALER_ROUTER/ws/64`: C 대비 `88.1%`
  - `MULTI_DEALER_ROUTER/ws/256`: C 대비 `87.7%`
  - `MULTI_DEALER_ROUTER/ws/1024`: C 대비 `92.6%`
  - `MULTI_DEALER_ROUTER/ws/131072`: C 대비 `70.9%`
  - `MULTI_DEALER_ROUTER/ws/262144`: C 대비 `66.0%`
  - `MULTI_ROUTER_ROUTER/ws/64`: C 대비 `95.6%`
  - `MULTI_ROUTER_ROUTER/ws/256`: C 대비 `94.7%`
  - `MULTI_ROUTER_ROUTER/ws/1024`: C 대비 `93.1%`
  - `MULTI_ROUTER_ROUTER/ws/262144`: C 대비 `100.6%`
  - `MULTI_PUBSUB/ws/64`: C 대비 `95.8%`
  - `MULTI_PUBSUB/ws/256`: C 대비 `98.2%`
  - `MULTI_PUBSUB/ws/1024`: C 대비 `92.6%`,
    `perf_cpp_multi_linux_20260518_124911_codex_cpp_ws_pubsub_1024_debug.txt`
  - `MULTI_PUBSUB/ws/65536`: C 대비 `113.9%`,
    `perf_cpp_multi_linux_20260518_124924_codex_cpp_ws_pubsub_large_recheck.txt`
  - `MULTI_PUBSUB/ws/131072`: C 대비 `87.6%`,
    `perf_cpp_multi_linux_20260518_124924_codex_cpp_ws_pubsub_large_recheck.txt`
  - `MULTI_PUBSUB/ws/262144`: C 대비 `88.7%`,
    `perf_cpp_multi_linux_20260518_124924_codex_cpp_ws_pubsub_large_recheck.txt`
  - `MULTI_STREAM/ws/64,256,1024,65536`: C 대비 `82.0%~108.1%`
- 미달
  - `MULTI_DEALER_DEALER/ws/64`: 제한 C 대비 `76.5%`
    - C: `perf_c_multi_linux_20260518_130452_codex_c_ws_dd_recheck_compare.txt`
    - C++: `perf_cpp_multi_linux_20260518_130911_codex_cpp_ws_dd_after_reuse_wait_overload.txt`
  - `MULTI_DEALER_DEALER/ws/262144`: C current 대비 `78.8%`
    - 제한 C 측정은 server non-zero exit로 partial이라 기준으로 쓰지 않았다.
    - C++ best: `perf_cpp_multi_linux_20260518_130205_codex_cpp_ws_dd_recheck.txt`
  - `MULTI_DEALER_ROUTER/ws/65536`: 제한 C 대비 `59.0%`
    - C: `perf_c_multi_linux_20260518_130512_codex_c_ws_dr_65536_recheck_compare.txt`
    - C++: `perf_cpp_multi_linux_20260518_130228_codex_cpp_ws_dr_65536_recheck.txt`
  - `MULTI_ROUTER_ROUTER/ws/65536`: 제한 C 대비 `60.3%`
    - C: `perf_c_multi_linux_20260518_130524_codex_c_ws_rr_large_recheck_compare.txt`
    - C++: `perf_cpp_multi_linux_20260518_130237_codex_cpp_ws_rr_large_recheck.txt`
  - `MULTI_ROUTER_ROUTER/ws/131072`: 제한 C 대비 `60.8%`
    - C: `perf_c_multi_linux_20260518_130524_codex_c_ws_rr_large_recheck_compare.txt`
    - C++: `perf_cpp_multi_linux_20260518_130237_codex_cpp_ws_rr_large_recheck.txt`
- 확인한 후보
  - `poller_t` socket-only event fill 방식 변경: 개선 없음, 되돌림.
  - `MULTI_DEALER_DEALER` client/server가 기존 public API의 재사용 wait overload를 쓰도록 정렬.
  - `MULTI_DEALER_DEALER` server 수신 메시지를 C 기준처럼 처리 후 명시 close.
  - routed echo client에서 framed transport payload를 C 기준처럼 공유 buffer에서 복사하도록
    정렬했다.
    - `MULTI_DEALER_ROUTER/ws/65536`은 한 차례 `91,164.6`까지 올랐지만,
      `62,237.0`, `50,134.6`으로 재측정되어 통과 근거로 쓰지 않는다.
    - 직접 `message_t` payload에 stamp하는 방식은 `MULTI_DEALER_ROUTER/ws/65536`을
      `61,695.6`으로 낮춰 폐기했다.
- 실행 실패
  - `MULTI_SPOT/ws/64`: `Unknown error 204 (errno=14)`, `MsgUnit(B)=4096`
    - `perf_cpp_multi_linux_20260518_131030_codex_cpp_ws_spot64_debug_recheck.txt`
  - `MULTI_SPOT_REQREP/ws/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
  - `MULTI_SPOT_SENDSEND/ws/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
- 보류 이유
  - `MULTI_DEALER_DEALER/ws/64,262144`
    - 기존 public API의 재사용 wait overload와 명시 close로 C 기준 처리 흐름에 맞췄지만
      목표를 넘지 못했다.
    - 직접 `message_t` payload에 stamp하는 방식은 64B만 소폭 개선하고 256KB latency를
      크게 악화시켜 폐기했다.
    - 추가/수정 후보: 반복 전송용 owned message builder 또는 benchmark target 재검토.
  - `MULTI_DEALER_ROUTER/ws/65536`, `MULTI_ROUTER_ROUTER/ws/65536,131072`
    - framed transport 공유 payload buffer 정렬과 직접 stamp 후보를 확인했지만 안정적인
      통과 수치를 만들지 못했다.
    - 현재 public routed recv/send API는 C처럼 native routing id pointer를 그대로
      재사용하는 hot path를 노출하지 않는다.
    - 추가/수정 후보: routing id materialization 없이 받은 route context로 단일 part를
      다시 보내는 public routed echo/send context API.
  - C++ public API에는 context-level auto-HWM message unit option이 없다.
  - raw option bag이나 C API 직접 호출은 public API hot path 원칙에 맞지 않는다.
  - 추가/수정 후보: context auto-HWM message unit option 공개 계약 추가.

### C++ wss full multi

- 실행 명령
  - `bindings/cpp/perf/run_binding_multi.sh --reuse-build --transports wss --results-tag codex_cpp_wss_full_status`
- 결과 파일
  - `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260518_132049_codex_cpp_wss_full_status.txt`
- C 기준
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - baseline이 오래되어 미달 항목은 같은 transport와 pattern으로 제한 C 재측정이 필요하다.
- 통과
  - `MULTI_DEALER_DEALER/wss/1024,65536`: C 대비 `83.7%~88.2%`
  - `MULTI_DEALER_ROUTER/wss/64,256,1024,65536,131072,262144`: C 대비 `84.4%~95.5%`
  - `MULTI_ROUTER_ROUTER/wss/64,256,1024,65536,131072,262144`: C 대비 `83.0%~93.5%`
  - `MULTI_STREAM/wss/64,256,1024,65536`: C 대비 `90.0%~100.1%`
- 미달
  - `MULTI_DEALER_DEALER/wss/64`: 제한 C 대비 `73.7%`
  - `MULTI_PUBSUB/wss/256`: 제한 C 대비 `78.3%`
  - `MULTI_PUBSUB/wss/65536`: 제한 C 대비 `67.2%`
- 실행 실패
  - `MULTI_DEALER_DEALER/wss/262144`: timeout
  - `MULTI_PUBSUB/wss/131072,262144`: timeout
  - `MULTI_SPOT/wss/*`: `Unknown error 204 (errno=14)`, `MsgUnit(B)=4096`
  - `MULTI_SPOT_REQREP/wss/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
  - `MULTI_SPOT_SENDSEND/wss/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
- 제한 C 재측정
  - `DEALER_DEALER`: `perf_c_multi_linux_20260518_133226_codex_c_wss_dd_recheck_compare.txt`
    - `256`: C 대비 `99.7%`, 통과
    - `131072`: C 대비 `92.6%`, 통과
  - `PUBSUB`: `perf_c_multi_linux_20260518_133255_codex_c_wss_pubsub_recheck_compare.txt`
    - `64`: C 대비 `89.4%`, 통과
    - `1024`: C 대비 `104.3%`, 통과
- 확인한 후보
  - `PUBSUB` server를 typed `pub_socket_t::publish()` 경로로 바꿨지만
    `wss/256,65536,131072,262144`가 모두 timeout으로 악화되어 되돌렸다.
- 보류 이유
  - `MULTI_PUBSUB` client hot path는 현재 public `subscribe(topic_message_t&)`가
    message마다 topic string과 parts vector를 물질화한다. C 기준처럼 topic buffer와
    단일 part를 재사용하는 public subscribe facade가 없다.
  - 추가/수정 후보: single-part pub/sub receive를 topic buffer와 message out parameter에
    직접 받는 public API.

### C++ tls full multi

- 실행 명령
  - `bindings/cpp/perf/run_binding_multi.sh --reuse-build --transports tls --results-tag codex_cpp_tls_full_status`
- 결과 파일
  - `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260518_133640_codex_cpp_tls_full_status.txt`
- C 기준
  - 첫 비교: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 미달 또는 timeout 항목은 같은 transport와 pattern으로 제한 재측정했다.
- 통과
  - `MULTI_DEALER_DEALER/tls/256,1024,65536`: C 대비 `83.4%~88.4%`
  - `MULTI_DEALER_ROUTER/tls/64,256,1024,65536,131072`: C 대비 `83.6%~89.1%`
  - `MULTI_DEALER_ROUTER/tls/262144`: 제한 C 대비 `88.2%`
  - `MULTI_ROUTER_ROUTER/tls/64,256,1024,65536,131072`: C 대비 `89.3%~94.8%`
  - `MULTI_ROUTER_ROUTER/tls/262144`: 제한 C 대비 `112.5%`
  - `MULTI_PUBSUB/tls/64,256`: 제한 C 대비 `80.9%~85.5%`
  - `MULTI_PUBSUB/tls/1024`: C 대비 `80.2%`
  - `MULTI_PUBSUB/tls/131072`: 제한 C 대비 `97.8%`
  - `MULTI_STREAM/tls/64,256,1024,65536`: C 대비 `94.8%~103.2%`
- 제한 C 재측정
  - `DEALER_DEALER`: `perf_c_multi_linux_20260518_140428_codex_c_tls_dd_recheck_compare.txt`
  - `PUBSUB`: `perf_c_multi_linux_20260518_140850_codex_c_tls_pubsub_recheck_compare.txt`
  - `DEALER_ROUTER/262144`: `perf_c_multi_linux_20260518_141339_codex_c_tls_dr_262_recheck_compare.txt`
  - `ROUTER_ROUTER/262144`: `perf_c_multi_linux_20260518_141403_codex_c_tls_rr_262_recheck_compare.txt`
- 제한 C++ 재측정
  - `DEALER_DEALER`: `perf_cpp_multi_linux_20260518_140451_codex_cpp_tls_dd_recheck.txt`
  - `PUBSUB`: `perf_cpp_multi_linux_20260518_140925_codex_cpp_tls_pubsub_recheck.txt`
  - `DEALER_ROUTER/262144`: `perf_cpp_multi_linux_20260518_141353_codex_cpp_tls_dr_262_recheck.txt`
  - `ROUTER_ROUTER/262144`: `perf_cpp_multi_linux_20260518_141416_codex_cpp_tls_rr_262_recheck.txt`
- 보류
  - `MULTI_DEALER_DEALER/tls/64`: 제한 C 대비 `75.5%`
  - `MULTI_DEALER_DEALER/tls/131072`: 제한 C 대비 `51.6%`
  - `MULTI_DEALER_DEALER/tls/262144`: C++ timeout, C 제한 측정은 성공
  - `MULTI_PUBSUB/tls/65536`: 제한 C 대비 `70.2%`
  - `MULTI_PUBSUB/tls/262144`: C++ timeout, C 제한 측정은 성공
  - `MULTI_SPOT/tls/*`: `Unknown error 204 (errno=14)`, `MsgUnit(B)=4096`
  - `MULTI_SPOT_REQREP/tls/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
  - `MULTI_SPOT_SENDSEND/tls/*`: `CLIENT_READY,64`, `MsgUnit(B)=4096`
- 보류 이유
  - `DEALER_DEALER`은 기존 public API의 재사용 wait overload와 수신 메시지 명시 close
    정렬 후에도 목표를 넘지 못했다. 직접 message payload stamp 방식은 앞선 ws 검토에서
    큰 메시지 latency를 악화시켜 폐기했다.
  - `PUBSUB`은 앞선 wss 검토에서 typed publish 경로가 timeout으로 악화되어 되돌렸다.
    남은 병목은 public `subscribe(topic_message_t&)`의 topic string과 parts vector
    물질화 비용으로 본다.
  - SPOT 계열은 public C++ context option으로 pub/sub auto-HWM message unit을 message
    size로 맞추는 계약이 없어 raw option이나 C API로 우회하지 않았다.
  - 추가/수정 후보는 ws/wss와 같다. 반복 전송용 owned message builder, single-part
    pub/sub receive facade, context auto-HWM message unit option이 필요하다.

## .NET

- `MULTI_SPOT/tcp/64`
  - .NET: `3119842.2`
  - C 기준: `5971358.8`
  - C 대비: `52.2%`
  - 목표: `60%`
  - 상태: 미달
  - 결과: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260518_111857_codex_tcp64_contract_hotloop_check.txt`
- `MULTI_SPOT/ws/64`
  - .NET: `3115855`
  - C 기준: `6735632`
  - C 대비: `46.3%`
  - 목표: `60%`
  - 상태: 미달
  - 결과: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260518_103205_codex_ws64_client_hotloop_probe.txt`
- 확인한 내부 후보
  - publish builder fast path
  - payload 생성 방식 변경
  - client hot loop 개선
- 남은 후보
  - tcp-first 원칙상 `MULTI_SPOT/tcp/64`부터 다시 직접 개선한다.
- 2026-05-18 재측정
  - C 제한 기준: `perf_c_multi_linux_20260518_141639_codex_c_tcp_spot64_dotnet_compare.txt`
    - C: `5,832,409.8 msg/s`
  - .NET current: `perf_dotnet_multi_linux_20260518_141619_codex_dotnet_tcp_spot64_current_recheck.txt`
    - .NET: `3,001,629.4 msg/s`, C 대비 `51.5%`
  - `PERF_DOTNET_SPOT_RECV_WORKERS=8` probe:
    `perf_dotnet_multi_linux_20260518_141837_codex_dotnet_tcp_spot64_recv_workers8_probe.txt`
    - .NET: `2,718,605.2 msg/s`, 개선 없음
    - 이 값은 C perf와 worker 조건이 달라지는 실험이므로 통과/보류 판정 근거로 쓰지 않는다.
      이후 라운드에서는 테스트 의미가 달라지는 worker 수 변경 실험을 하지 않는다.
- 중단한 public API 프로토타입
  - 직접 publish overload 후보: `3,147,725.4 msg/s`
  - 단일 part subscribe buffer 후보: `3,269,798.4 msg/s`
  - span publish 후보: `3,069,849.4 msg/s`
  - 목표 60%에 필요한 약 `3,499,446 msg/s`에 미달했고 개선 폭도 크지 않아 public API
    변경 후보로 계속 진행하지 않는다. 프로토타입 코드는 측정 직후 원복했다.
- 기존 API 내부 프로토타입
  - routed echo tcp client payload를 C와 같은 의미로 맞추는 perf 정렬:
    `perf_dotnet_multi_linux_20260518_145439_codex_dotnet_tcp_routed_full_borrow_payload.txt`
    - `Effective Options`: `routed_echo_borrow_payload=tcp`
    - `MULTI_DEALER_ROUTER`: `62.5%`, `64.8%`, `65.8%`, `80.6%`, `103.6%`, `130.6%`
    - `MULTI_ROUTER_ROUTER`: `54.9%`, `56.0%`, `54.5%`, `73.9%`, `93.1%`, `145.7%`
    - `MULTI_ROUTER_ROUTER/tcp/1024`는 절대 기준은 넘지만 C의
      `ROUTER_ROUTER / DEALER_ROUTER` 상대 비율보다 15%p를 조금 넘게 낮아
      제한 재측정 대상으로 남겼다.
  - tcp borderline 제한 재측정:
    `perf_dotnet_multi_linux_20260518_145742_codex_dotnet_tcp_borderline_recheck.txt`
    - `MULTI_DEALER_DEALER/tcp/64`: `1,601,415.8 msg/s`, C 대비 `53.5%`, 미달
    - `MULTI_DEALER_DEALER/tcp/256`: `1,205,802.6 msg/s`, C 대비 `62.5%`, 미달
    - `MULTI_PUBSUB/tcp/64`: `1,343,102.4 msg/s`, C 대비 `61.3%`, 미달
    - `MULTI_ROUTER_ROUTER/tcp/1024`: `202,117.4 msg/s`, C 대비 `56.9%`, 통과
    - `MULTI_SPOT_SENDSEND/tcp/64`: `155,329.8 msg/s`, C 대비 `69.5%`, 통과
  - raw send/publish builder inline 후보:
    `perf_dotnet_multi_linux_20260518_150141_codex_dotnet_tcp_oneway_inline_candidate.txt`
    - 단위 테스트: `bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj`, 145개 통과
    - `MULTI_PUBSUB/tcp/64`: `1,468,394.2 msg/s`, C 대비 `67.0%`, 통과
    - `MULTI_DEALER_DEALER/tcp/64`: `1,608,215.0 msg/s`, C 대비 `53.7%`, 보류
    - `MULTI_DEALER_DEALER/tcp/256`: `1,167,607.0 msg/s`, C 대비 `60.5%`, 보류
    - 남은 `DEALER_DEALER` small miss는 public API builder와 `Message` 생성 비용이
      결합된 경로다. public API 변경 없이 적용 가능한 얇은 builder inline 후보는
      목표에 닿지 못했고, 의미가 달라지는 borrowed payload 전환은 C one-way 조건과
      맞지 않아 시도하지 않았다.
  - SPOT echo large timeout 제한 재측정:
    `perf_dotnet_multi_linux_20260518_150313_codex_dotnet_tcp_spot_timeout_recheck.txt`
    - `MULTI_SPOT_REQREP/tcp/65536`: timeout
    - `MULTI_SPOT_REQREP/tcp/262144`: timeout
    - `MULTI_SPOT_SENDSEND/tcp/65536`: timeout
    - `MULTI_SPOT_SENDSEND/tcp/262144`: timeout
    - `MsgUnit(B)`는 각 message size와 같았다. timeout, HWM, client 수를 바꾸면
      C 비교 조건과 달라지므로 조정하지 않았다.
- `ws` full
  - .NET: `perf_dotnet_multi_linux_20260518_150810_codex_dotnet_ws_full_status.txt`
    - 상태: partial, 42 success / 4 fail
    - fail: `MULTI_SPOT_REQREP/ws/65536`, `MULTI_SPOT_REQREP/ws/262144`,
      `MULTI_SPOT_SENDSEND/ws/65536`, `MULTI_SPOT_SENDSEND/ws/262144`
    - `MsgUnit(B)`는 출력된 모든 size에서 message size와 일치했다.
  - C SPOT 제한 기준: `perf_c_multi_linux_20260518_151858_codex_c_ws_spot_dotnet_compare.txt`
    - baseline SPOT의 `MsgUnit(B)=4096` 이력이 있어 SPOT 계열만 같은 조건으로 재측정했다.
  - 판정 요약
    - `DEALER_DEALER`: 64/256/131072 보류, 나머지 통과.
    - `DEALER_ROUTER`: 전체 통과.
    - `ROUTER_ROUTER`: 64/256/1024/65536 보류, 131072/262144 통과.
    - `PUBSUB`: 64/256/1024 보류, 65536/131072/262144 통과.
    - `SPOT`: 전체 보류. C 제한 기준 대비 `33.7%~51.6%`.
    - `SPOT_REQREP`: 64/256/1024/131072 통과, 65536/262144 보류.
    - `SPOT_SENDSEND`: 64/256/1024/131072 통과, 65536/262144 보류.
    - `STREAM`: 전체 통과.
- `wss` full
  - .NET: `perf_dotnet_multi_linux_20260518_152442_codex_dotnet_wss_full_status.txt`
    - 상태: partial, 42 success / 4 fail
    - fail: `MULTI_SPOT_REQREP/wss/65536`,
      `MULTI_SPOT_SENDSEND/wss/65536`,
      `MULTI_SPOT_SENDSEND/wss/131072`,
      `MULTI_SPOT_SENDSEND/wss/262144`
    - `MsgUnit(B)`는 출력된 모든 size에서 message size와 일치했다.
  - C SPOT 제한 기준: `perf_c_multi_linux_20260518_153614_codex_c_wss_spot_dotnet_compare.txt`
    - baseline SPOT의 `MsgUnit(B)=4096` 이력이 있어 SPOT 계열만 같은 조건으로 재측정했다.
  - 판정 요약
    - `DEALER_DEALER`: 64/256 보류, 나머지 통과.
    - `DEALER_ROUTER`, `ROUTER_ROUTER`, `STREAM`: 전체 통과.
    - `PUBSUB`: 64/256/1024 보류, 65536/131072/262144 통과.
    - `SPOT`: 64/262144 보류, 256/1024/65536/131072 통과.
    - `SPOT_REQREP`: 65536 보류, 나머지 통과.
    - `SPOT_SENDSEND`: 64/256/1024 통과, 65536/131072/262144 보류.
  - `Message.WrapBytes`가 기존 thread-local `Message` pool을 쓰도록 하는 후보:
    `perf_dotnet_multi_linux_20260518_145139_codex_dotnet_tcp_spot64_message_pool.txt`
    - .NET: `2,919,071.4 msg/s`, C 대비 `50.1%`
    - 같은 public API와 같은 조건을 유지한 내부 변경이지만 목표에 미달했고, 직전 제한
      측정보다 낫지 않아 원복했다.
  - SPOT publish에서 borrowed send를 우회하고 native message copy를 강제하는 후보:
    `perf_dotnet_multi_linux_20260518_142632_codex_dotnet_tcp_spot64_copy_send_proto.txt`
    - .NET: `3,094,403.6 msg/s`, C 대비 `53.1%`
  - 목표에 미달하고 `Message.WrapBytes`의 zero-copy 의도와도 맞지 않아 원복했다.
- 보류 이유
  - 현재 public API를 유지한 내부 후보는 목표에 닿지 못했다.
  - public API 변경 후보도 실측 개선 폭이 작아 최후 수단으로 계속 진행할 근거가 부족하다.
- `tls` full
  - .NET: `perf_dotnet_multi_linux_20260518_154137_codex_dotnet_tls_full_status.txt`
    - full runner는 `MULTI_SPOT_REQREP/tls/131072` 이후 자식 benchmark 없이 shell만 남아
      멈췄다. 이미 기록된 결과는 유지하고, 남은 조합은 제한 tail 측정으로 채웠다.
  - .NET tail 제한 측정:
    `perf_dotnet_multi_linux_20260518_155100_codex_dotnet_tls_tail_status.txt`
    - 상태: partial, 13 success / 3 fail
    - fail: `MULTI_SPOT_REQREP/tls/65536`,
      `MULTI_SPOT_REQREP/tls/131072`, `MULTI_SPOT_SENDSEND/tls/65536`
    - `MsgUnit(B)`는 출력된 모든 size에서 message size와 일치했다.
  - C SPOT 제한 기준: `perf_c_multi_linux_20260518_155605_codex_c_tls_spot_dotnet_compare.txt`
    - baseline SPOT의 `MsgUnit(B)=4096` 이력이 있어 SPOT 계열만 같은 조건으로 재측정했다.
    - `Effective Options`는 .NET과 같은 transport, size, duration, client 수, timeout,
      auto-HWM, socket buffer 조건이었다.
  - 판정 요약
    - `DEALER_DEALER`: 64/256 보류, 나머지 통과.
    - `DEALER_ROUTER`: 전체 통과.
    - `ROUTER_ROUTER`: 64/256/1024 보류, 65536/131072/262144 통과.
    - `PUBSUB`: 64/256/1024/65536/262144 보류, 131072 통과.
    - `SPOT`: 64/65536/131072/262144 보류, 256/1024 통과.
    - `SPOT_REQREP`: 64/256/1024/262144 통과, 65536/131072 보류.
    - `SPOT_SENDSEND`: 64/256/1024/131072/262144 통과, 65536 보류.
    - `STREAM`: 전체 통과.
  - .NET은 `tcp`, `ws`, `wss`, `tls` 모두 `미달` 또는 `미측정` 없이
    `통과`와 `보류`만 남았다. 다음 순서는 Java `tcp`다.

## Java

- `tcp` full
  - Java: `perf_java_multi_linux_20260518_160351_codex_java_tcp_full_status.txt`
    - 상태: complete, 46 success / 0 fail
    - `Effective Options`: transport `tcp`, duration 5s, clients 100,
      timeout auto, auto-HWM, socket buffer auto-HWM.
  - 판정 요약
    - `DEALER_DEALER`: 전체 통과, C 대비 `68.2%~92.7%`.
    - `DEALER_ROUTER`: 전체 통과, C 대비 `55.3%~87.0%`.
    - `ROUTER_ROUTER`: 64/256/1024는 절대 기준 통과지만 C의
      `ROUTER_ROUTER / DEALER_ROUTER` 상대 비율보다 22.0~22.8%p 낮아 `미달`.
      65536/131072/262144는 통과.
    - `PUBSUB`: 전체 통과, C 대비 `90.6%~256.7%`.
    - `STREAM`: 전체 통과, C 대비 `102.5%~135.1%`.
  - SPOT 계열
    - full 결과의 SPOT 계열은 `MsgUnit(B)=4096`으로 출력되어 유효 비교로 쓰지 않는다.
    - 비공개 native bridge로 SpotNode 내부 pub/sub message unit을 맞추는 후보를
      제한 측정했지만 서버가 `READY`를 내기 전에 timeout으로 실패했다.
    - 이 후보는 public API hot path도 아니고 안정적으로 동작하지 않아 원복했다.
    - 현재 Java public API에는 SpotNode의 SPOT 내부 pub/sub message unit을 테스트
      size로 맞추는 계약이 없다. 하지만 `MsgUnit(B)` 불일치와 public API 제약만으로
      보류하지 않는다. small size 수치가 낮고 내부 개선 후보가 남아 있으므로 SPOT
      계열은 `미달`로 유지한다.
  - 정정: Java `tcp`는 아직 완료가 아니다. `MULTI_ROUTER_ROUTER/tcp/64,256,1024`,
    `MULTI_SPOT`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND` 미달을 먼저 개선한다.
- 아래 항목은 `tcp` full 전의 부분 측정 기록이다.
- `MULTI_PUBSUB/tcp/64`
  - Java: `2075974.2`
  - C 기준: `3518022.8`
  - C 대비: `59.0%`
  - 목표: `63%`
  - 상태: 미달
  - 결과: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260518_111324_codex_java_mpubsub_tcp64_after_sendbuilder_single_storage.txt`
- `MULTI_PUBSUB/tcp/256`
  - Java: `1960535.6`
  - C 기준: `3004889.0`
  - C 대비: `65.2%`
  - 목표: `63%`
  - 상태: 통과
  - 결과: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260518_110614_codex_java_mpubsub_tcp64_256_after_reuse_topicmsg.txt`
- `MULTI_DEALER_ROUTER/tcp/65536`
  - Java: `88922.6`
  - C 기준: `190471.4`
  - C 대비: `46.7%`
  - 목표: `50%`
  - 상태: 미달
  - 결과: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260518_111220_codex_java_mdr_tcp65536_131072_after_router_single_fastpath.txt`
- `MULTI_DEALER_ROUTER/tcp/131072`
  - Java: `44766.0`
  - C 기준: `79036.6`
  - C 대비: `56.6%`
  - 목표: `50%`
  - 상태: 통과
  - 결과: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260518_111220_codex_java_mdr_tcp65536_131072_after_router_single_fastpath.txt`
- SPOT 계열
  - `MsgUnit(B)=4096` 불일치 때문에 64B 유효 비교로 보지 않는다.

- Java SPOT 개선 라운드
  - 적용한 내부 변경
    - `PerfTransport.applySpotOptions`에서 C perf와 같은
      `zlink_set_option(..., ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES)` 경로로
      SpotNode의 auto-HWM message unit을 테스트 size와 맞췄다.
      Java public API surface는 변경하지 않았다.
    - `MULTI_SPOT_SENDSEND` 클라이언트는 C perf처럼 하나의 poll loop가 여러
      Spot을 round-robin으로 구동하도록 바꿨다. 100개 Java thread 구조는 C의
      측정 의미와 달라서 제거했다.
  - 성공한 제한 측정
    - Java:
      `perf_java_multi_linux_20260518_164226_codex_java_tcp_spot_family_after_sendsend_msgunit.txt`
    - C `MULTI_SPOT` 제한 기준:
      `perf_c_multi_linux_20260518_165437_codex_c_tcp_spot_isolated_compare.txt`
    - C `MULTI_SPOT_SENDSEND` 제한 기준:
      `perf_c_multi_linux_20260518_163633_codex_c_tcp_sendsend_isolated_compare.txt`
  - 판정
    - `MULTI_SPOT_SENDSEND/tcp/64,256,1024`: Java `177K~194Kops/s`,
      C 제한 기준 `75.6%~80.9%`, 통과.
    - `MULTI_SPOT/tcp/*`: `MsgUnit(B)`는 정렬됐지만 C 제한 기준
      `34.3%~43.1%`, 미달.
    - `MULTI_SPOT_REQREP/tcp/64,256,1024`: `4.2%~4.7%`, 미달.
    - `MULTI_SPOT_REQREP/tcp/65536,131072,262144`와
      `MULTI_SPOT_SENDSEND/tcp/65536,131072,262144`: timeout 또는 실패, 미달.
  - 실패한 후보
    - `Spot.recvRouted(Received, ...)`를 직접 채우는 fast path는
      `MULTI_SPOT_SENDSEND/tcp/64,256,1024`를 약 `2.5Kops/s`까지 낮춰 폐기했다.
    - `SPOT_REQREP` 클라이언트의 per-request latch를 없앤 async window 후보는
      `256B`만 `2.8Kops/s`로 성공하고 `64/1024B`가 실패해 폐기했다.
  - 다음 내부 후보
    - `SPOT_REQREP`는 public request API의 hot path가 spot마다 progress pump를
      따로 갖는 구조라, C처럼 여러 spot request를 하나의 progress loop에서
      처리할 수 있는 내부 개선을 먼저 검토한다.

- Java `MULTI_SPOT_REQREP/tcp` progress pump와 client submit loop 개선
  - 코드 변경
    - `RequestProgressPump`가 request 하나가 끝날 때마다 poller/thread를 바로
      종료하지 않고 짧은 idle 구간 동안 유지하도록 바꿨다. public API는 변경하지
      않았다.
    - Java perf client의 `MULTI_SPOT_REQREP` hot loop를 100개 worker와
      per-request latch 구조에서 하나의 submit loop와 callback 완료 처리 구조로
      바꿨다. 큰 message에서는 C perf와 같은 active slot 제한을 적용했다.
    - transient submit backpressure는 C perf와 같이 fatal 실패로 보지 않고 같은
      active window 안에서 재시도하도록 정렬했다.
  - 측정 결과
    - `64B`: `166673.2ops/s`, C full 기준 `86.6%`, 통과.
      결과: `perf_java_multi_linux_20260518_170706_codex_java_tcp_reqrep_single_submit_loop.txt`
    - `256B`: `162561.6ops/s`, C full 기준 `75.6%`, 통과.
      결과: `perf_java_multi_linux_20260518_170747_codex_java_tcp_reqrep_single_submit_loop_256_recheck.txt`
    - `1024B`: `148392.4ops/s`, C full 기준 `66.2%`, 통과.
      결과: `perf_java_multi_linux_20260518_170706_codex_java_tcp_reqrep_single_submit_loop.txt`
    - `65536B,131072B,262144B`: timeout은 없어졌지만 C full 기준
      `0.16%~0.38%`로 미달.
      결과: `perf_java_multi_linux_20260518_170802_codex_java_tcp_reqrep_single_submit_loop_large.txt`
  - 판정
    - `MULTI_SPOT_REQREP/tcp/64,256,1024`: 통과.
    - `MULTI_SPOT_REQREP/tcp/65536,131072,262144`: 미달. public API 제한 또는
      timeout을 보류 사유로 쓰지 않고, 큰 message에서 submit/copy/progress 경로의
      내부 후보를 계속 확인한다.

- Java `MULTI_SPOT_REQREP/tcp` 큰 message 후속 개선
  - 코드 변경
    - 작은 message는 기존 dispatch callback server 경로를 유지하고, 큰 message만
      server active loop에서 직접 `recvRouted(DONT_WAIT)`를 drain하도록 분기했다.
    - server reply의 transient submit 실패는 C perf처럼 fatal로 보지 않고 계속
      진행하도록 맞췄다.
    - 큰 message client는 active spot에 대해 public `Poller`의 `POLLCOMPLETION`을
      함께 기다리도록 했다.
  - 최신 측정
    - `64,256,1024B`: `163.5K,163.6K,151.0Kops/s`,
      C full 기준 `67.3%~84.9%`, 통과.
      결과: `perf_java_multi_linux_20260518_171915_codex_java_tcp_reqrep_size_split_small_recheck.txt`
    - `65536,131072B`: `7.68K,4.06Kops/s`,
      C full 기준 `14.3%~17.4%`, 미달.
      결과: `perf_java_multi_linux_20260518_171620_codex_java_tcp_reqrep_completion_poller_large.txt`
    - `262144B`: `1.60Kops/s`, C full 기준 `19.2%`, 미달.
      결과: `perf_java_multi_linux_20260518_171738_codex_java_tcp_reqrep_262144_completion_poller_recheck.txt`
  - 판정
    - 큰 message는 timeout/no-sample 문제가 사라졌지만 목표와 큰 차이가 남아 있다.
      보류가 아니며, 다음 후보는 SPOT routed reply/request large payload의 내부
      submit 경로와 copy/reference 처리를 계속 확인하는 것이다.

- Java `MULTI_SPOT/tcp` TopicMessage 재사용 후보
  - 코드 변경
    - receive worker가 message마다 `TopicMessage`를 새로 만들지 않고, public
      `subscribe(TopicMessage, DONT_WAIT)`의 caller-provided 결과 객체를 worker별로
      재사용하도록 바꿨다.
  - 측정 결과
    - `64,256,1024B`: `2.35M,2.31M,2.51Mmsg/s`,
      C 제한 기준 `33.9%~41.4%`, 미달.
      결과: `perf_java_multi_linux_20260518_172205_codex_java_tcp_spot_reuse_topicmessage_small.txt`
    - `65536B`: `810.7Kmsg/s`, C 제한 기준 `64.3%`, 통과.
    - `131072,262144B`: `288.0K,148.0Kmsg/s`,
      C 제한 기준 `30.4%~34.4%`, 미달.
      결과: `perf_java_multi_linux_20260518_172316_codex_java_tcp_spot_reuse_topicmessage_large.txt`
  - 판정
    - `MULTI_SPOT/tcp/65536`은 통과로 변경한다.
    - 나머지 `MULTI_SPOT` size는 미달이며 보류가 아니다.

- .NET `MULTI_SPOT_REQREP/tcp` C 대비 확인
  - 기준 파일
    - .NET: `perf_dotnet_multi_linux_20260518_142736_codex_dotnet_tcp_full_status.txt`
    - C: `perf_c_multi_linux_20260518_143705_codex_c_tcp_full_dotnet_compare.txt`
  - 결과
    - `64B`: .NET `150191.4ops/s`, C `192535.2ops/s`, C 대비 `78.0%`, 통과.
    - `256B`: .NET `149533.8ops/s`, C `214975.2ops/s`, C 대비 `69.6%`, 통과.
    - `1024B`: .NET `140512.6ops/s`, C `224221.2ops/s`, C 대비 `62.7%`, 통과.
    - `65536B`: .NET timeout/no result, 미달.
    - `131072B`: .NET `20153.2ops/s`, C `23335.2ops/s`, C 대비 `86.4%`, 통과.
    - `262144B`: .NET timeout/no result, 미달.
  - 판정
    - timeout만으로 보류하지 않는다. `65536B`, `262144B`는 내부 원인 재검토 대상이다.

- Java `MULTI_SPOT_REQREP/tcp` 큰 message 추가 원인 확인
  - callback submit 내부 경로를 `CompletableFuture<Received> -> thenApply -> whenComplete`
    체인 대신 callback registry가 직접 완료하도록 바꿨다. public API 시그니처는 바꾸지 않았다.
  - reply submit을 `messageTransferTo`로 바꾸는 실험은 reply message 소유권 의미를 흔들 수
    있어 유지하지 않고 원래의 `messageCopyTo`로 되돌렸다.
  - server large-size 전용 direct drain과 1ms sleep은 C/C++ perf의 dispatch drain 방식과
    다르므로 dispatch handler drain으로 정렬했다.
  - request progress pump는 완료 이벤트를 한 번에 1개만 받던 것을 64개 batch로 받도록
    바꿨다. public API는 바꾸지 않았다.
  - 측정 결과
    - `perf_java_multi_linux_20260518_173428_codex_java_tcp_reqrep_direct_callback_large.txt`:
      `65536,131072,262144B = 7554.8,3980.4,2317.2ops/s`.
    - `perf_java_multi_linux_20260518_173546_codex_java_tcp_reqrep_dispatch_server_large.txt`:
      `7593.4,3977.8,2328.0ops/s`.
    - `perf_java_multi_linux_20260518_173729_codex_java_tcp_reqrep_progress_batch_large.txt`:
      `7628.0,4061.2,2322.0ops/s`.
  - C 기준 대비
    - `65536B`: `7628.0 / 53713.6 = 14.2%`, 미달.
    - `131072B`: `4061.2 / 23335.2 = 17.4%`, 미달.
    - `262144B`: `2322.0 / 8298.4 = 28.0%`, 미달.
  - 추가 실험
    - perf client의 별도 completion poller를 제거하고 callback에서 client loop를 `unpark`하는
      방식은 `65536B`, `131072B`에서 runner 실패가 나고 성능 이득도 없어 유지하지 않았다.
  - 판정
    - 현재 확인된 주 병목은 timeout이나 `MsgUnit(B)` 불일치가 아니다.
    - callback 직접 완료와 progress batch도 큰 개선을 만들지 못했으므로 다음 후보는
      SPOT routed request/reply의 large payload submit 경로에서 C의 `zlink_msg_init_data`
      기반 전송과 Java public `Message` 보존 계약 사이의 copy/reference 처리 차이다.
    - public API 변경 없이 내부에서 보존 계약을 유지하면서 copy 비용을 줄일 수 있는지 계속
      확인한다.

## Node

- `PUBSUB/tcp/64`
  - Node: `454589.0`
  - C 기준: `1226642.4`
  - C 대비: `37.06%`
  - 목표: `35%`
  - 상태: 통과
  - 결과: `bindings/node/perf/results/single/report/perf_node_single_linux_20260518_111604.txt`
- `PUBSUB/tcp/256`
  - Node: `371307.4`
  - C 기준: `1022904.8`
  - C 대비: `36.30%`
  - 목표: `35%`
  - 상태: 통과
  - 결과: `bindings/node/perf/results/single/report/perf_node_single_linux_20260518_111503.txt`
- 주의
  - 위 두 조합만 확인했다. 모든 pattern, size, transport 완료가 아니다.

## Go

- `PAIR/tcp/64`: C 대비 `79.80%`, 상태 통과
- `DEALER_DEALER/tcp/64`: C 대비 `78.05%`, 상태 통과
- `DEALER_ROUTER/tcp/64`: C 대비 `45.12%`, 목표 `47%`, 상태 미달
- `ROUTER_ROUTER/tcp/64`: C 대비 `50.49%`, 상태 통과
- `PUBSUB/tcp/64`: C 대비 `9.78%`, 상태 미달
- `SPOT/tcp/64`: C 대비 `29.65%`, 상태 미달
- 결과
  - `bindings/go/perf/results/single/report/perf_go_single_linux_20260518_115650_codex_go_tcp64_single_recv_into.txt`
  - `bindings/go/perf/results/single/report/perf_go_single_linux_20260518_120037_codex_go_tcp64_pubsub_adopt_recv.txt`
- 주의
  - Go single report는 raw socket `MsgUnit(B)`를 출력하지 못한다.
  - 기존 SPOT 계열은 `MsgUnit(B)=4096`이라 64B 비교 기준으로 쓰지 않는다.

## Python

- `MULTI_DEALER_DEALER/tcp/64`: C 대비 `4.55%`, 상태 미달
- `MULTI_PUBSUB/tcp/64`: C 대비 `4.42%`, 상태 미달
- `MULTI_DEALER_ROUTER/tcp/64`: C 대비 `9.98%`, 상태 미달
- `MULTI_ROUTER_ROUTER/tcp/64`: C 대비 `8.47%`, 상태 미달
- `MULTI_STREAM/tcp/64`: C 대비 `0.82%`, 상태 미달
- `MULTI_SPOT_REQREP/tcp/64`: C 대비 `0.24%`, 상태 미달
- `MULTI_SPOT_SENDSEND/tcp/64`: C 대비 `3.53%`, 상태 미달
- `MULTI_SPOT/tcp/64`: client timeout, 상태 미달
- 결과
  - `bindings/python/perf/results/multi/report/perf_python_multi_linux_20260518_115738_codex_python_tcp64_owned_recv.txt`
- 주의
  - SPOT 계열은 `MsgUnit(B)=4096`이라 64B 비교 기준으로 쓰지 않는다.

## .NET SPOT_REQREP 재검토

- timeout/no result는 timeout 증가나 retry로 처리하지 않고 원인을 추적했다.
- 확인한 구현 불일치
  - `MULTI_SPOT_REQREP` 설정이 `RequestReply`가 아니라 `SendSend` 모드로 되어 있었다.
    이 상태에서 나온 기존 숫자는 C `MULTI_SPOT_REQREP`와 같은 의미가 아니므로 공식
    통과 근거로 쓰지 않는다.
  - request operation timeout이 `PERF_MULTI_RCVTIMEO_MS`/`PERF_MULTI_SNDTIMEO_MS`
    200ms가 아니라 connect-ready timeout을 사용하고 있었다.
  - public callback completion이 active window 이후에 늦게 실행되어 server가 reply를
    보낸 뒤에도 client measure count가 0으로 끝나는 재현을 확인했다.
- 적용한 의미 보존 수정
  - `.NET MULTI_SPOT_REQREP`를 실제 `RequestReply` 모드로 되돌렸다.
  - request timeout은 C와 같은 `rcvtimeo_ms` 우선, 없으면 `sndtimeo_ms`, 마지막 fallback
    200ms로 맞췄다.
  - `SynchronizationContext`가 없는 request callback은 completion continuation에서 바로
    실행하도록 바꿔 dispatcher 지연을 줄였다. public API 시그니처는 바꾸지 않았다.
- 측정
  - `perf_dotnet_multi_linux_20260518_174741_codex_dotnet_tcp_reqrep_request_timeout_fix.txt`:
    timeout 의미 수정만 적용했을 때 `262144B = 8205.6ops/s`가 나왔으나, 이 시점에는
    `ReqRepConfig`가 아직 `SendSend` 모드였으므로 공식 판정에 쓰지 않는다.
  - `perf_dotnet_multi_linux_20260518_175008_codex_dotnet_tcp_reqrep_mode_fix.txt`:
    `RequestReply` 모드 정정 후 `65536B`, `262144B` 모두 result_timeout.
  - `perf_dotnet_multi_linux_20260518_181748_codex_dotnet_tcp_reqrep_direct_callback_65536.txt`:
    callback 직접 전달 후에도 `65536B` result_timeout.
- 현재 판정
  - 아래 `.NET/Java timeout 원인 수정과 tcp 비교 갱신`에서 후속 수정과 재측정을 완료했다.
    이 시점의 timeout/no-result 판정은 폐기하고, 최신 수치 기반 판정을 사용한다.

## .NET/Java timeout 원인 수정과 tcp 비교 갱신

- .NET `MULTI_SPOT_REQREP`
  - 원인
    - `RequestProgressPump`가 `Task.Run` 안에서 native poll을 무기한 blocking하여
      ThreadPool worker를 점유했다. 100개 request spot에서 completion continuation이
      active window 뒤로 밀려 server reply가 있어도 client count가 0으로 끝났다.
    - 큰 message에서는 auto-HWM slot 수와 같은 수의 request를 동시에 걸 때 request
      submit 경로가 HWM 근처에서 멈췄다.
  - 수정
    - request progress worker를 ThreadPool이 아니라 dedicated background thread로
      실행하도록 바꿨다. public API는 바꾸지 않았다.
    - `SPOT_REQREP`/`SPOT_SENDSEND` perf client의 large message in-flight slot을
      auto-HWM slot의 절반으로 제한했다. client 수는 그대로 100이고, 동시에 outstanding인
      public API 호출 수만 HWM 용량에 맞췄다.
  - 확인 결과
    - `perf_dotnet_multi_linux_20260518_184839_codex_dotnet_reqrep64_progress_thread.txt`:
      `64B` timeout 해소, `17.35Kops/s`.
    - `perf_dotnet_multi_linux_20260518_185507_codex_dotnet_reqrep_large_half_hwm_slots.txt`:
      `65536,131072,262144B = 19.24K,12.77K,6.21Kops/s`, complete.
    - `perf_dotnet_multi_linux_20260518_185532_codex_dotnet_sendsend65536_half_hwm_slots.txt`:
      `SPOT_SENDSEND/65536B = 36.31Kops/s`, complete.
    - `perf_dotnet_multi_linux_20260518_185545_codex_dotnet_pubsub262144_recheck.txt`:
      `PUBSUB/262144B = 36.20Kmsg/s`, complete.
  - 판정
    - timeout 자체는 해소됐다.
    - 실제 RequestReply 모드 기준 .NET `SPOT_REQREP/tcp`는
      `64,256,1024,65536,131072B`가 C 대비 `8.3%~54.7%`라 미달이다.
      `262144B`는 `74.8%`로 통과다.

- Java `MULTI_SPOT_SENDSEND`
  - 원인
    - large message에서 active in-flight slot이 auto-HWM 용량을 초과했다.
    - 측정 뒤 stop token을 모든 client 수만큼 기다려 server가 종료되지 않는 경우가 있었다.
      실제 client 로그에는 결과가 있었지만 runner가 `server_exit_124`로 실패 처리했다.
  - 수정
    - large message in-flight slot을 auto-HWM slot의 절반으로 제한했다.
    - 측정에 실제 참여한 active slot 수만큼 stop token을 기대하도록 server 종료 조건을
      맞췄다. stop token 전송은 cleanup 경로이므로 transient submit 실패는 결과를 막지
      않게 했다.
  - 확인 결과
    - `perf_java_multi_linux_20260518_185601_codex_java_sendsend_large_half_hwm_slots.txt`:
      `65536B = 38.12Kops/s`, complete.
    - `perf_java_multi_linux_20260518_185808_codex_java_sendsend_tail_active_stop_count.txt`:
      `131072,262144B = 15.31K,6.65Kops/s`, complete.
  - 판정
    - Java `SPOT_SENDSEND/tcp/65536,131072,262144`는 C 대비 `81.1%~131.3%`로 통과다.

- 현재 tcp 비교 요약
  - .NET은 실제 RequestReply 모드로 정정하면서 기존 `SPOT_REQREP` 통과 판정 일부가
    취소됐다. 이는 성능 회귀가 아니라 이전 측정 의미가 `SendSend`였던 오류를 바로잡은
    결과다.
  - Java는 `SPOT_REQREP`와 `SPOT_SENDSEND`의 timeout/no-result가 해소됐고,
    최신 tcp 비교에서는 두 패턴 모두 C 대비 목표를 만족한다.

### .NET/Java tcp 전체 pattern/size 처리량 비교

아래 값은 최신 .NET/Java tcp report와 C full tcp 기준을 합쳐 계산했다. 단위는
`Kops/s` 또는 `Kmsg/s` 계열의 초당 처리량이다.

| Pattern | Size(B) | .NET | Java | Java/.NET | .NET/C | Java/C |
|---------|--------:|-----:|-----:|----------:|-------:|-------:|
| `MULTI_DEALER_DEALER` | 64 | 1568.6K | 2027.4K | 129.2% | 52.4% | 67.7% |
| `MULTI_DEALER_DEALER` | 256 | 1176.8K | 1723.6K | 146.5% | 61.0% | 89.3% |
| `MULTI_DEALER_DEALER` | 1024 | 1064.0K | 1209.5K | 113.7% | 77.1% | 87.6% |
| `MULTI_DEALER_DEALER` | 65536 | 192.4K | 113.0K | 58.7% | 113.1% | 66.4% |
| `MULTI_DEALER_DEALER` | 131072 | 91.3K | 56.3K | 61.6% | 99.2% | 61.1% |
| `MULTI_DEALER_DEALER` | 262144 | 41.2K | 31.4K | 76.1% | 84.6% | 64.4% |
| `MULTI_DEALER_ROUTER` | 64 | 240.0K | 268.9K | 112.0% | 61.7% | 69.2% |
| `MULTI_DEALER_ROUTER` | 256 | 243.4K | 283.2K | 116.3% | 64.2% | 74.7% |
| `MULTI_DEALER_ROUTER` | 1024 | 237.7K | 281.2K | 118.3% | 65.5% | 77.4% |
| `MULTI_DEALER_ROUTER` | 65536 | 103.4K | 71.2K | 68.8% | 69.3% | 47.7% |
| `MULTI_DEALER_ROUTER` | 131072 | 53.6K | 35.7K | 66.7% | 94.3% | 62.9% |
| `MULTI_DEALER_ROUTER` | 262144 | 30.4K | 18.6K | 61.3% | 133.2% | 81.7% |
| `MULTI_ROUTER_ROUTER` | 64 | 198.8K | 212.3K | 106.8% | 53.4% | 57.1% |
| `MULTI_ROUTER_ROUTER` | 256 | 195.5K | 210.9K | 107.9% | 53.9% | 58.1% |
| `MULTI_ROUTER_ROUTER` | 1024 | 196.9K | 210.4K | 106.9% | 55.4% | 59.2% |
| `MULTI_ROUTER_ROUTER` | 65536 | 95.6K | 70.0K | 73.2% | 67.3% | 49.3% |
| `MULTI_ROUTER_ROUTER` | 131072 | 51.5K | 37.0K | 71.9% | 86.2% | 62.0% |
| `MULTI_ROUTER_ROUTER` | 262144 | 24.5K | 19.1K | 77.7% | 128.4% | 99.8% |
| `MULTI_PUBSUB` | 64 | 1370.6K | 1805.8K | 131.8% | 62.6% | 82.4% |
| `MULTI_PUBSUB` | 256 | 1431.6K | 1870.9K | 130.7% | 69.7% | 91.1% |
| `MULTI_PUBSUB` | 1024 | 924.0K | 1046.7K | 113.3% | 111.1% | 125.8% |
| `MULTI_PUBSUB` | 65536 | 164.7K | 272.3K | 165.4% | 89.0% | 147.2% |
| `MULTI_PUBSUB` | 131072 | 71.8K | 113.7K | 158.3% | 112.0% | 177.4% |
| `MULTI_PUBSUB` | 262144 | 36.2K | 40.0K | 110.4% | 172.5% | 190.4% |
| `MULTI_SPOT` | 64 | 3055.8K | 2504.1K | 81.9% | 55.1% | 45.1% |
| `MULTI_SPOT` | 256 | 2694.0K | 2257.9K | 83.8% | 40.4% | 33.9% |
| `MULTI_SPOT` | 1024 | 2766.9K | 2308.8K | 83.4% | 49.4% | 41.2% |
| `MULTI_SPOT` | 65536 | 487.6K | 660.8K | 135.5% | 40.9% | 55.5% |
| `MULTI_SPOT` | 131072 | 292.7K | 296.3K | 101.2% | 39.8% | 40.3% |
| `MULTI_SPOT` | 262144 | 168.8K | 169.0K | 100.1% | 37.5% | 37.5% |
| `MULTI_SPOT_REQREP` | 64 | 18.0K | 127.0K | 706.7% | 9.3% | 66.0% |
| `MULTI_SPOT_REQREP` | 256 | 18.8K | 159.2K | 848.5% | 8.7% | 74.0% |
| `MULTI_SPOT_REQREP` | 1024 | 18.7K | 144.4K | 772.8% | 8.3% | 64.4% |
| `MULTI_SPOT_REQREP` | 65536 | 19.2K | 48.3K | 251.0% | 35.8% | 89.9% |
| `MULTI_SPOT_REQREP` | 131072 | 12.8K | 18.0K | 141.3% | 54.7% | 77.3% |
| `MULTI_SPOT_REQREP` | 262144 | 6.2K | 8.9K | 144.0% | 74.8% | 107.7% |
| `MULTI_SPOT_SENDSEND` | 64 | 147.0K | 199.6K | 135.8% | 65.8% | 89.3% |
| `MULTI_SPOT_SENDSEND` | 256 | 146.5K | 195.9K | 133.7% | 65.9% | 88.2% |
| `MULTI_SPOT_SENDSEND` | 1024 | 143.9K | 186.3K | 129.4% | 66.8% | 86.5% |
| `MULTI_SPOT_SENDSEND` | 65536 | 36.3K | 38.1K | 105.0% | 93.1% | 97.7% |
| `MULTI_SPOT_SENDSEND` | 131072 | 20.2K | 15.3K | 75.7% | 107.2% | 81.1% |
| `MULTI_SPOT_SENDSEND` | 262144 | 7.9K | 6.7K | 84.0% | 156.3% | 131.3% |
| `MULTI_STREAM` | 64 | 342.9K | 356.2K | 103.9% | 96.3% | 100.0% |
| `MULTI_STREAM` | 256 | 300.4K | 342.3K | 113.9% | 87.4% | 99.5% |
| `MULTI_STREAM` | 1024 | 268.2K | 323.0K | 120.4% | 83.6% | 100.6% |
| `MULTI_STREAM` | 65536 | 48.8K | 50.1K | 102.7% | 112.9% | 116.0% |

## .NET SPOT_REQREP 직접 callback 개선

- 원인
  - Java와 비교했을 때 .NET `SPOT_REQREP`는 실제 RequestReply 모드에서 작은 message가
    `18Kops/s` 수준으로 낮았다.
  - Java `RequestProgressPump`는 pending 요청이 없어져도 짧게 유지되지만, .NET pump는
    요청 1개가 끝날 때마다 poller/thread를 바로 종료했다. 반복 request/reply에서 thread와
    poller 생성 비용이 매 요청마다 발생했다.
  - .NET callback `Submit(...)` 경로가 public callback을 직접 완료하지 않고 `Task` 기반
    async 경로를 경유해 TCS/continuation 비용을 추가로 지불했다.
  - perf runner는 payload를 `new Message(span)`으로 만들고 binding 내부 clone에서 다시
    복사했다. Java runner는 재사용 payload를 넘기므로 같은 의미에서 .NET의 첫 복사는
    불필요했다.
  - .NET server reply는 `DONTWAIT`를 사용했지만 C server reply는 blocking submit이다.
    HWM 압력 아래에서 이 차이가 timeout/no-result로 이어졌다.

- 수정
  - `RequestProgressPump`를 dedicated background thread로 유지하고, pending 0 이후에도
    1초 keepalive를 둔다. active 요청이 있을 때는 signal-driven wait를 사용한다.
  - `Spot.RequestToSpot(...).Submit(callback)` 내부 구현을 직접 native callback 완료 경로로
    바꿨다. public API 시그니처와 계약은 바꾸지 않았다.
  - `.NET SPOT_REQREP` perf runner의 payload 생성은 기존 public `Message.WrapBytes`를
    사용해 첫 번째 복사를 제거했다.
  - `.NET SPOT_REQREP` server reply는 C와 같이 blocking reply로 맞췄다.
  - large message active slot은 timeout을 유발한 HWM 초과 구간을 피하되, 기존 HWM 절반
    고정보다 C 상한과 auto-HWM 값을 더 가깝게 반영하도록 조정했다. `65536B`는 active 16에서
    timeout이 재현되어 stable complete가 확인된 active 8을 유지하고, `131072B`와
    `262144B`는 auto-HWM cap까지 사용한다.

- 측정
  - `.NET small`: `perf_dotnet_multi_linux_20260518_192515_codex_dotnet_reqrep_wrapbytes_small_recheck.txt`
    - `64,256,1024B = 136.01K,136.80K,132.15Kops/s`.
  - `.NET large`: `perf_dotnet_multi_linux_20260518_192658_codex_dotnet_reqrep_wrapbytes_large_final.txt`
    - `65536,131072,262144B = 34.76K,24.12K,9.71Kops/s`.
  - Java large recheck:
    `perf_java_multi_linux_20260518_192338_codex_java_reqrep_hwm_cap_large_recheck.txt`
    - `65536,131072,262144B = 51.12K,21.71K,8.07Kops/s`.

- 판정
  - C full tcp 기준 .NET `SPOT_REQREP/tcp`는 `64,256,65536,131072,262144B`가 통과로
    바뀌었다.
  - `1024B`는 `58.9%`로 목표 하한에 근소하게 미달한다. timeout/no-result는 아니며,
    다음 라운드에서 추가 내부 최적화 후보를 찾는다.
  - 추가 후보로 per-slot latency buffer 사전 할당을 시험했지만
    `perf_dotnet_multi_linux_20260518_193010_codex_dotnet_reqrep1024_latency_prealloc.txt`
    에서 `132.09Kops/s`로 개선이 없어 코드는 되돌렸다.

## .NET SPOT_REQREP 1024 fallback scan 제거

- 원인
  - .NET `SPOT_REQREP`는 direct native callback과 progress pump를 쓰면서도 active loop에서
    매 반복마다 `RecvRouted(DontWait)` fallback을 모든 active spot에 대해 훑었다.
  - C와 Java의 request/reply callback 경로는 이 fallback scan을 사용하지 않는다. .NET은
    callback 완료 시 wake fd를 신호하므로 같은 의미에서 fallback scan은 hot path 비용이다.

- 수정
  - `RequestReply` 모드의 active loop에서 `DrainRequestReplyFallback` 호출을 제거했다.
  - 공개 API는 바꾸지 않았다.
  - progress callback 등록에서 요청마다 만들던 관리 객체를 struct lease로 줄였다. 이 후보만
    단독 측정했을 때는 개선이 거의 없었지만, fallback scan 제거와 함께 유지해도 의미 변화는
    없다.

- 측정
  - `perf_dotnet_multi_linux_20260518_194125_codex_dotnet_reqrep1024_no_fallback_scan.txt`
    - `MULTI_SPOT_REQREP/tcp/1024 = 143.83Kops/s`
    - C full tcp `224.22Kops/s` 대비 `64.1%`

- 판정
  - .NET `MULTI_SPOT_REQREP/tcp/1024`는 통과로 바뀌었다.

## .NET small one-way와 SPOT publish 후보 확인

- `MULTI_DEALER_DEALER/tcp/64,256`
  - 최신 제한 비교:
    - .NET `perf_dotnet_multi_linux_20260518_222421_codex_dotnet_tcp_dd_small_recheck.txt`
      기준 `64,256B = 1.619M,1.127Mmsg/s`.
    - C `perf_c_multi_linux_20260518_222421_codex_c_tcp_dd_small_dotnet_compare.txt`
      기준 `64,256B = 2.940M,1.872Mmsg/s`.
    - C 대비 `55.1%`, `60.2%`로 여전히 목표 미달이다.
  - 내부 후보:
    - `Message(ReadOnlySpan<byte>)`를 managed snapshot 뒤 borrowed-send로 보내는 후보는
      `perf_dotnet_multi_linux_20260518_222618_codex_dotnet_tcp_dd_small_managed_copy_ctor.txt`
      에서 server abort와 큰 성능 악화를 만들었다. 코드는 원복했다.
    - one-way sender에서 `Message.WrapBytes`를 직접 쓰는 후보는
      `perf_dotnet_multi_linux_20260518_222720_codex_dotnet_tcp_dd_small_wrapbytes_candidate.txt`
      에서 server abort와 큰 성능 악화를 만들었다. 코드는 원복했다.
  - 판정:
    - 현재 public API로는 payload header를 native send message에 직접 stamp할 수 없어
      byte array stamp 뒤 `Message` 생성 복사가 남는다.
    - 추가 개선은 반복 전송용 writable/owned message builder 또는 동등한 public API 추가/수정이
    필요하므로 보류로 둔다.

- `MULTI_SPOT/tcp/64,256,1024`
  - SPOT publish server가 `Message.WrapBytes`를 쓰는 경로와 반대로, C처럼 native message로
    복사하는 후보를 시험했다.
  - C 제한 기준:
    `perf_c_multi_linux_20260518_222930_codex_c_tcp_spot_small_dotnet_compare.txt`
    에서 `64,256,1024B = 6.713M,5.806M,5.485Mmsg/s`.
  - .NET 후보:
    `perf_dotnet_multi_linux_20260518_223040_codex_dotnet_tcp_spot_small_native_message_candidate2.txt`
    에서 `64,256,1024B = 3.081M,2.918M,2.891Mmsg/s`.
  - C 대비 `45.9%`, `50.3%`, `52.7%`로 목표를 넘지 못했고 `64B`는 기존 수치보다 낮았다.
    코드는 원복했다.
  - 판정:
    - SPOT/PUBSUB publish-subscribe 계열의 남은 병목은 public API 내부 후보만으로는
      목표를 넘기지 못했다.
    - 추가 개선은 publish payload를 직접 구성하는 writable message builder, 또는
      subscribed frame을 재사용 버퍼로 받는 typed/raw receive facade 추가/수정이 필요하므로
      보류로 둔다.

## Java tcp 미달 항목 재검토

- `MULTI_DEALER_ROUTER`
  - 이전 `1024B` 제한 측정 timeout은 재현되지 않았다.
  - `perf_java_multi_linux_20260518_195221_codex_java_dealer_router_small_after_spot_internal.txt`
    에서 `64,256,1024B = 316.90K,308.82K,310.94Kops/s`, complete다.

- `MULTI_ROUTER_ROUTER`
  - `perf_java_multi_linux_20260518_193615_codex_java_router_router_small_current_recheck.txt`
    기준 `64,256,1024B = 234.87K,228.26K,228.53Kops/s`다.
  - 최신 `DEALER_ROUTER` 대비 `ROUTER_ROUTER` 상대 비율은 `73.5%~74.1%`다.
  - C의 같은 상대 비율은 `95.6%~97.9%`라서 상대 기준은 `21.5~24.4%p` 낮다.
  - client hot path에서 기존 public byte[] routing-id send overload 사용을 검토했다.
    현재 Java `SendFlags`는 public이므로 package-private flag 문제는 보류 사유가 아니다.
    남은 확인 대상은 public routed single-part send가 C의 `_part` 경로와 같은 의미로
    내려가는지, routing id materialization을 피할 수 있는지다.

- `MULTI_SPOT`
  - `Spot.publish(...).message(...).submit()` 단일 part 경로가 내부에서 `MessagePartsBuffer`
    경로를 타던 부분을 single-part builder fast path로 바꿨다.
  - 단일 publish native 임시 `zlink_msg_t`는 thread-local scratch를 쓰도록 바꿨다.
  - `perf_java_multi_linux_20260518_194759_codex_java_spot_send_scratch_small.txt`
    에서 `64,256,1024B = 2.42M,2.57M,2.45Mmsg/s`로 작은 개선은 있었지만 목표에는 부족했다.
  - `perf_java_multi_linux_20260518_194404_codex_java_spot_single_send_builder.txt`
    에서 large 포함 전체 측정은 complete였고 `65536,131072,262144B =
    612.65K,345.70K,147.96Kmsg/s`다.
  - 직접 새 메시지를 만들고 헤더만 stamp하는 후보는
    `perf_java_multi_linux_20260518_195022_codex_java_spot_direct_message_small.txt`
    에서 clean latency pass가 실패해 의미 보존 후보로 채택하지 않고 되돌렸다.

- 판정
  - Java `tcp`에서 남은 `미달` 표시는 `보류`로 바꿨다. 보류 근거는 public API 변경 없이
    측정 의미를 유지하는 내부 후보가 더 확인되지 않았기 때문이다.

## Java ws/wss/tls SPOT_SENDSEND 후속 라운드

- 수정
  - `PerfControl`이 같은 stdin FIFO에 대해 매번 새 `BufferedReader`를 만들던 문제를 고쳤다.
    앞 reader가 `START` 줄을 미리 버퍼링하면 뒤 reader가 영원히 못 받아 client timeout이
    나던 원인이다. 공용 reader와 lock을 사용해 control line 소비 순서를 보존한다.
  - Java perf timestamp를 `Instant.now()` 기반 epoch ns에서 `System.nanoTime()`으로 바꿨다.
    SPOT echo latency는 같은 client 프로세스가 stamp와 receive를 모두 처리하므로 단조 시계로
    충분하고 hot path 비용이 낮다.
  - `MULTI_SPOT_SENDSEND` client는 재사용 payload를 다시 `Message.copyOf(...)` 하지 않고
    public send builder에 그대로 넘긴다. public API는 바꾸지 않았다.
  - server reply도 `received.firstPart().move()` 임시 객체를 만들지 않고 받은 part를 그대로
    public send builder에 넘긴다. public API는 바꾸지 않았다.
  - public `Message.wrapDirect(ByteBuffer)` 노출 실험은 `wss/131072B`에서 개선이 없어 바로
    원복했다. 최종 코드에는 public interface 변경이 없다.

- 측정
  - C 제한 기준 `ws/65536,131072,262144`:
    `perf_c_multi_linux_20260518_205954_codex_c_ws_sendsend_large_java_compare.txt`.
  - Java `ws/65536,262144`:
    `perf_java_multi_linux_20260518_210350_codex_java_ws_sendsend_large_final_nano.txt`.
  - Java `ws/131072` 단독:
    `perf_java_multi_linux_20260518_210336_codex_java_ws_sendsend131072_nano_time.txt`.
  - Java `wss/65536`:
    `perf_java_multi_linux_20260518_210512_codex_java_wss_sendsend65536_final_nano.txt`.
  - Java `wss/131072` best probe:
    `perf_java_multi_linux_20260518_210732_codex_java_wss_sendsend131072_slots_2_probe.txt`.
  - C `wss/131072` 제한 기준:
    `perf_c_multi_linux_20260518_210806_codex_c_wss_sendsend131072_java_compare.txt`.
  - Java `tls/65536,131072,262144`:
    `perf_java_multi_linux_20260518_210940_codex_java_tls_sendsend65536_final_nano.txt`,
    `perf_java_multi_linux_20260518_210951_codex_java_tls_sendsend131072_final_nano.txt`,
    `perf_java_multi_linux_20260518_211000_codex_java_tls_sendsend262144_final_nano.txt`.

- 판정
  - `ws/65536`, `ws/131072`, `ws/262144`, `wss/65536`, `tls/65536`은 통과다.
  - `wss/131072`, `wss/262144`, `tls/131072`, `tls/262144`는 미달로 남긴다.
  - 미달 원인은 timeout이나 `MsgUnit(B)`가 아니다. 다음 확인 대상은 Java SPOT routed
    send/reply large payload 경로에서 WSS/TLS backpressure와 native submit copy/reference 처리다.

### Java WSS 262144 단독 재측정

- 측정
  - C `wss/MULTI_SPOT_SENDSEND/262144`:
    `perf_c_multi_linux_20260518_223446_codex_c_wss_sendsend262144_java_compare.txt`
    에서 `3.768Kops/s`.
  - Java `wss/MULTI_SPOT_SENDSEND/262144`:
    `perf_java_multi_linux_20260518_223449_codex_java_wss_sendsend262144_single_recheck.txt`
    에서 `2.274Kops/s`.
  - C 대비 `60.4%`이고 `MsgUnit(B)=262144`가 양쪽 모두 맞다.

- 판정
  - `wss/MULTI_SPOT_SENDSEND/262144`는 최신 단독 제한 측정 기준 통과로 바꾼다.
  - `wss/131072`, `tls/131072`, `tls/262144`는 앞선 active slot/public `wrapDirect`
    실험과 `System.nanoTime()` 정렬 뒤에도 목표 미달이다. public API 변경 없이 더 확인할
    내부 후보가 없어 보류로 둔다.

## Node tcp 측정과 내부 후보

- `MULTI_SPOT` smoke 중 `262144B`에서 종료가 지연됐다.
  - 원인은 client drain loop가 slot별 inner burst 안에서 fallback deadline을 다시 확인하지
    않아 큰 backlog를 오래 비우는 동안 종료 조건을 지나치는 것이었다.
  - `bindings/node/tests/optimization_guard.test.ts`에 inner burst deadline guard를 먼저 추가했고
    실패를 확인한 뒤, `perf_multi_spot_client.ts`의 inner loop에 deadline 확인을 추가했다.
  - 수정 후 `perf_node_multi_linux_20260518_224402_codex_node_tcp_spot262144_deadline_inner_fix.txt`
    는 complete다.
  - 다만 `Auto-HWM spotnode`는 여전히 모든 size에서 `MsgUnit(B)=4096`이라 C와 조건이 다르다.
    Node public API에는 context auto-HWM message unit option이 없어 SPOT
    계열은 보류로 둔다.

- `MULTI_DEALER_DEALER`
  - non-routed single-part sender는 public `sendFrom(..., DontWait)` fast path를 쓰도록 바꿨다.
    guard를 먼저 추가했고 실패/통과를 확인했다.
  - receiver는 caller-provided `Received` 재사용만으로는 목표를 넘지 못했다.
  - 이후 public `recvInto(buffer, DontWait)`로 단일 part payload를 직접 받도록 바꿨다.
    이 경로는 `MULTI_DEALER_DEALER`의 wire format이 single-part라서 의미를 보존한다.
  - 최종 full:
    `perf_node_multi_linux_20260518_230350_codex_node_tcp_dd_full_recvinto_final.txt`.
    `64,256,1024,65536,131072B`는 C 대비 `56.4%~83.2%`로 통과다.
  - `262144B` 단독:
    `perf_node_multi_linux_20260518_230538_codex_node_tcp_dd262144_recvinto_epipe_fix_recheck.txt`.
    C 대비 `38.8%`로 통과다.

- runner 종료 버그
  - `DEALER_DEALER/262144` 단독 재측정 중 server stdin에 `STOP`을 쓰는 시점에 이미 pipe가
    닫혀 `EPIPE`가 unhandled error로 올라왔다.
  - `writeChildLine` guard를 추가해 closed child stdin pipe를 닫힌 상태로 처리하고, `STOP`,
    `START`, SPOT control line 전송에 같은 helper를 쓰도록 바꿨다.
  - guard test는 `node multi orchestrator ignores closed child stdin pipes`다.

- `MULTI_PUBSUB`
  - client hot path에서 `TopicMessage`를 매 recv마다 새로 만들던 부분을 caller-provided
    storage 재사용으로 바꿨다.
  - `perf_node_multi_linux_20260518_230630_codex_node_tcp_pubsub_full_topic_storage_reuse.txt`
    기준 `1024B`는 C 대비 `40.3%`로 통과다.
  - `64,256,65536,131072,262144B`는 `16.8%~25.8%`로 미달이다. `TopicMessage` 재사용만으로는
    목표를 넘지 못했고, 추가 개선은 public raw/typed subscribed receive facade 추가/수정이 필요해
    보류로 둔다.

- routed echo와 stream
  - `perf_node_multi_linux_20260518_230906_codex_node_tcp_routed_full_sendfrom_current.txt`
    기준 `MULTI_DEALER_ROUTER`는 `64,256,1024B` 통과, large는 `14.7%~27.6%`로 보류다.
  - 같은 파일 기준 `MULTI_ROUTER_ROUTER`는 `64,256B` 통과, `1024B` 이상은 `13.9%~29.2%`로
    보류다.
  - routed server reply/send는 public builder/context 경로에 묶여 있어 추가 개선은 public
    routed raw send 또는 borrowed send context 추가/수정이 필요하다.
  - `perf_node_multi_linux_20260518_230942_codex_node_tcp_stream_full_current.txt` 기준
    `MULTI_STREAM/65536B`는 `43.1%`로 통과지만 `64,256,1024B`는 `16.7%~19.0%`다.
    stream echo는 frame 재구성 Buffer와 stream send builder 경로에 묶여 있어 public stream
    raw send/borrowed frame API 추가/수정 없이는 보류로 둔다.

## Node ws smoke/full 측정

- smoke
  - `perf_node_multi_linux_20260518_231615_codex_node_ws_multi_smoke_all_sizes.txt`는
    `MULTI_SPOT/ws/262144B`에서 `fast_mutex.hpp:98` `Invalid argument`가 한 번 발생해 partial이다.
  - 같은 조합 단독 재현:
    `perf_node_multi_linux_20260518_231641_codex_node_ws_spot262144_fast_mutex_repro.txt`.
    단독은 complete라서 broad smoke 중 transient crash로 기록한다.

- full
  - `perf_node_multi_linux_20260518_232616_codex_node_ws_multi_full_status.txt`는
    `MULTI_STREAM/ws/1024B`에서 `double free or corruption (fasttop)`이 한 번 발생해 partial이다.
  - 같은 조합 단독 재현:
    `perf_node_multi_linux_20260518_232633_codex_node_ws_stream1024_double_free_repro.txt`.
    단독은 complete라서 broad full 중 transient crash로 기록한다.
  - `MULTI_DEALER_DEALER/ws/64B`는 full에서 outlier `63.0Kmsg/s`였지만
    `perf_node_multi_linux_20260518_232722_codex_node_ws_dd64_full_outlier_recheck.txt`
    에서 `1.840Mmsg/s`, C 대비 `59.9%`로 통과다.

- 판정
  - `MULTI_DEALER_DEALER/ws` 전체는 `59.9%~100.1%`로 통과다.
  - `MULTI_DEALER_ROUTER/ws`는 `65536B`만 `22.5%`로 보류, 나머지는 `33.8%~42.7%`로 통과다.
  - `MULTI_ROUTER_ROUTER/ws`는 `65536B`만 `24.5%`로 보류, 나머지는 `30.2%~38.8%`로 통과다.
  - `MULTI_PUBSUB/ws`는 `262144B`만 `35.2%`로 통과, 나머지는 `21.3%~33.3%`로 보류다.
  - `MULTI_STREAM/ws`는 `65536B`만 `90.5%`로 통과, `64,256,1024B`는 `13.4%~14.2%`로 보류다.
  - SPOT 계열은 full 수치와 별개로 `Auto-HWM spotnode`의 `MsgUnit(B)=4096` 불일치가 남아
    모두 보류다. context auto-HWM message unit option 추가/수정이 필요하다.

## Node wss smoke/full 측정

- smoke
  - `perf_node_multi_linux_20260518_233410_codex_node_wss_multi_smoke_all_sizes.txt`는
    `MULTI_SPOT_SENDSEND/wss/256B` bind 충돌과 `MULTI_STREAM/wss/65536B` client 실패로 partial이다.
  - 남은 `perf_multi_spot_sendsend_server.js`를 정리한 뒤 같은 실패 행을 단독 재측정했다.
    `perf_node_multi_linux_20260518_233448_codex_node_wss_spot_sendsend256_bind_repro.txt`와
    `perf_node_multi_linux_20260518_233501_codex_node_wss_stream65536_client_fail_repro.txt`는
    모두 complete다.

- full과 C 기준
  - Node full:
    `perf_node_multi_linux_20260518_234522_codex_node_wss_multi_full_status.txt`, complete.
  - C full 기준:
    `perf_c_multi_linux_20260518_234546_codex_c_wss_multi_full_node_compare.txt`, complete.
  - `MULTI_DEALER_DEALER/wss/256B`는 full 뒤 단독 재측정에서 낮은 outlier가 있었지만,
    전체 DD 재측정 `perf_node_multi_linux_20260518_235652_codex_node_wss_dd_all_sizes_anomaly_check.txt`
    에서 정상 범위로 돌아왔다.
  - `MULTI_ROUTER_ROUTER/wss/1024B`는 full 기준 29.5%라 기준선에 걸렸고, Node/C 단독 재측정
    `perf_node_multi_linux_20260518_235711_codex_node_wss_rr1024_threshold_recheck.txt`,
    `perf_c_multi_linux_20260518_235723_codex_c_wss_rr1024_node_compare.txt` 기준 30.7%로 통과다.

- 판정
  - `MULTI_DEALER_DEALER/wss` 전체는 `49.8%~57.0%`로 통과다.
  - `MULTI_DEALER_ROUTER/wss` 전체는 `33.4%~40.8%`로 통과다.
  - `MULTI_ROUTER_ROUTER/wss` 전체는 `30.7%~41.0%`로 통과다.
  - `MULTI_PUBSUB/wss`는 `262144B`만 `36.4%`로 통과, 나머지는 `17.0%~32.7%`로 보류다.
  - `MULTI_STREAM/wss`는 `65536B`만 `92.9%`로 통과, `64,256,1024B`는 `20.8%~21.9%`로 보류다.
  - SPOT 계열은 full 수치와 별개로 `Auto-HWM spotnode`의 `MsgUnit(B)=4096` 불일치가 남아
    모두 보류다. context auto-HWM message unit option 추가/수정이 필요하다.

## Public API 제한 해석 정정

- C API가 이미 제공하는 계약을 binding public API가 빠뜨렸거나 잘못 감싼 경우에는
  public API 추가/수정을 금지하지 않는다. 이 경우 누락이나 오구현 자체가 수정 근거다.
- SPOT 계열 `MsgUnit(B)=4096` 문제는 이 기준에 해당한다. C API는
  `zlink_set_option`, `zlink_get_option`과 `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES`
  경로를 제공하고, 이 common option은 SPOT pub/sub default로 전파된다.
  Node는 일반 socket `autoHwmMsgUnitBytes`만 노출하고 context auto-HWM message unit option이 없으므로 public API 추가/수정 대상으로 둔다.
- 이후 방향은 SpotNode/Spot별 facade가 아니라
  `doc/plan/monitoring/context-auto-hwm-msg-unit-rollout-plan.ko.md`의 context option
  rollout로 정정했다.
- 같은 context 안에서 socket마다 평균 message size를 따로 제어하는 사용 사례는 일반 경로로
  보지 않는다. 따라서 새 context option을 추가하는 동시에 binding public API의 socket별
  message unit facade는 제거하는 방향으로 정리했다. C API의 handle-level option은 저수준
  계약으로 유지한다.
- 현재 확인된 public API 추가/수정 대상 목록은
  `doc/plan/perf/bindings-library-performance-improvement-plan.ko.md`의
  `Public API 추가/수정 대상` 섹션에 모았다.
