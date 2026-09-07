# core-rf-R9-ABC 요약

worktree: ~/project/zlink-work/r9 (detached 80871f34f3). 커밋하지 않음(diff만).

## 결과
- 순삭제 460줄 / 신규 82줄 (net -378줄), 12개 파일.
- dev 빌드(JOBS=4) 통과, 경고 없음.
- ctest -R 'tcp|ipc|tls|ws|wss|endpoint|listener|connect|release|transport' (53개) 5회 전부 100% 통과.
- test_endpoint_release 10회 단독 실행 전부 rc=0.
- C 바인딩: bindings/c/tests의 cmake를 core/build-dev(dev lib)에 대해 수동 구성(run_tests.sh는 core/build(release)를 기본 가정하므로 -DZLINK_C_CORE_BUILD_DIR=core/build-dev로 직접 configure/build) → 10개 계약 테스트 전부 통과, 빌드 수 초. run_tests.sh 원본 스크립트 자체는 실행하지 않았음(release 경로 하드코딩) — 대신 그 스크립트가 쓰는 것과 동일한 cmake 플래그 세트를 dev 빌드 디렉터리로 재지정해 실행.

## 변경 파일
- core/src/runtime/transports/tcp/tcp.{hpp,cpp} — tcp_write/tcp_read/tcp_open_socket 삭제(-170/-24줄), 부수적으로 미사용된 `#include "core/options.hpp"`와 전방선언 2개도 제거.
- core/src/runtime/transports/tls/ssl_context_helper.{hpp,cpp} — create_server_context_from_pem / create_client_context_from_pem / create_client_context_with_cert_from_pem / load_ca_certificate_from_pem 4개 삭제(연쇄, -165/-28줄), 미사용된 `<boost/asio/buffer.hpp>` 제거.
- core/src/runtime/transports/asio/asio_listener_accept_policy.hpp — 신규 템플릿 헬퍼 2개 추가: `prepare_asio_listener_termination`(process_term 공통 시퀀스: linger 저장→release_endpoint→drain_asio_listener_pending_accepts→own_process_term, 콜백 주입), `cancel_asio_listener_accept_if_terminating`(on_accept의 "terminating 이면 열린 소켓 닫고 true 반환" 블록).
- tcp/asio_tcp_listener.cpp, ipc/asio_ipc_listener.cpp, tls/asio_tls_listener.cpp, ws/asio_ws_listener.cpp — process_term과 on_accept/on_tcp_accept의 해당 블록을 위 헬퍼 호출로 교체. 로그 매크로(LISTENER_DBG/IPC_LISTENER_DBG/TLS_LISTENER_DBG/WS_LISTENER_DBG) 이름과 로그 문구는 헬퍼 밖에 그대로 남겨 트랜스포트별 트레이스 문구 차이를 유지.
- tls/wss_address.cpp, ws/ws_address.hpp/.cpp — `wss_address_t::to_string`이 새 protected `ws_address_t::format_url(scheme_, addr_)`를 호출하도록 통합(family 분기·host/port/path 조합 로직 1곳으로).

## 설계 비교와 선택 이유
- 묶음 B: (1) 매크로로 공통 블록 뽑기 vs (2) 기존 `prepare_asio_connecter_termination`(asio_timer_flag.hpp)과 동일한 "콜백 주입 템플릿 함수" 패턴. (2) 선택 — connecter 쪽과 대칭적인 이름·구조를 유지하고, 매크로보다 타입 체크가 되며, 로그 매크로 차이(트랜스포트별 문구)를 호출부에 남겨 관찰 가능성을 그대로 보존할 수 있음. 새 헬더는 만들지 않고 이미 같은 목적(accept 정책)의 `asio_listener_accept_policy.hpp`에 추가(제안 문서의 신규 헤더안 대신 기존 파일 재사용 — 파일 수 최소화).
- 묶음 C: (1) 자유함수 헬퍼 vs (2) protected 멤버 `format_url`. (2) 선택 — wss가 이미 ws를 상속하므로 protected 접근자 하나만 추가하면 되고, family()/host()/port()/path() 등 protected/public 멤버에 자연스럽게 접근 가능. 자유함수안은 4개 접근자를 인자로 다시 넘겨야 해서 오히려 결합이 늘어남.

## 실행한 테스트와 남은 실패
- ctest 필터 5회: 53/53 통과 x5, 실패 없음.
- test_endpoint_release 10회: 10/10 rc=0.
- C 바인딩 계약 테스트(dev lib 대상): 10/10 통과.
- 남은 실패 없음.

## 성능
- 해당 없음(죽은 코드 삭제·구조적 리팩터만, 핫패스 함수 변경 없음). 벤치 미실행(브리프 범위에 성능 확인 항목 없음).

## 재확인한 스펙 절
- 08-posd-module-structure.ko.md, 07-core-source-layout.ko.md 기준 재확인: 공개 헤더(core/include/**) 무변경, libzlink.vers 무변경. process_term/on_accept 리팩터는 호출 순서(linger 저장→release_endpoint→drain pending accepts→own_t::process_term / 감소→terminating 체크→소켓 닫기→return)를 100% 보존 — 어느 문장도 다른 동작이 되지 않았다.

## 변경 분류
- B(기존 결함/중복 정리): 4건 모두 죽은 코드 제거 또는 중복 로직 통합, 계약·동작 변화 없음.

## 멈춘 지점
없음. 3개 묶음(A/B/C) 전부 적용 완료.
