# Phase 3 인벤토리 — R9 (transports/{tcp,tls,ws,ipc,asio})

기준 문서: doc/principal/dev/posddd.ko.md, core/doc/spec/core/systems/08-posd-module-structure.ko.md, 07-core-source-layout.ko.md.
Repo HEAD 482d7bca80. 읽기 전용 조사(빌드/수정/커밋 없음). 대상 45개 파일, 총 9,270행.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1 dead | tls/ssl_context_helper.cpp:69 `create_server_context_from_pem` | 리포 전체 grep 결과 호출자 0(내부/외부 모두). from_options 계열만 실사용됨 | 함수 삭제(선언+정의) | 2파일, ~45줄 | 없음(내부 헬퍼, 외부 인클루드 참조 없음 확인) | 없음 |
| 2 | 1 dead | tls/ssl_context_helper.cpp:156 `create_client_context_from_pem` | 상동, 호출자 0 | 삭제 | 2파일, ~40줄 | 없음 | 없음 |
| 3 | 1 dead | tls/ssl_context_helper.cpp:258 `create_client_context_with_cert_from_pem` | 상동, 호출자 0 | 삭제 | 2파일, ~55줄 | 없음 | 없음 |
| 4 | 1 dead | tls/ssl_context_helper.cpp:440 `load_ca_certificate_from_pem` | 위 3개 죽은 함수에서만 호출(연쇄 죽은 코드), 다른 호출자 없음 | #1~3과 함께 삭제 | 상동에 포함 | 없음 | 없음 |
| 5 | 1 dead | tcp/tcp.cpp:170 `tcp_write` | 리포 전체 grep 호출자 0. 구식(pre-asio) 원시소켓 경로 잔재로 추정 | 삭제. asio_tcp_transport 계열이 실제 read/write 담당 | tcp.hpp/.cpp 2파일, ~56줄 | 없음(내부 함수, 공개 API 아님) | 없음 |
| 6 | 1 dead | tcp/tcp.cpp:226 `tcp_read` | 상동, 호출자 0 | 삭제 | 상동, ~45줄 | 없음 | 없음 |
| 7 | 1 dead | tcp/tcp.cpp:309 `tcp_open_socket` | 상동, 호출자 0. tcp_address/tcp_transport 모두 asio acceptor/connector를 직접 씀 | 삭제. **확인 필요**: 헤더 export가 바인딩/플랫폼별 조건부 코드에서 쓰이는지 CMake 조건별 재확인(현재 grep은 활성 소스 트리 기준) | 상동, ~67줄 | 없음 | 없음 |
| 8 | 2 중복 | tcp/asio_tcp_listener.cpp:121-135, ipc/asio_ipc_listener.cpp:210-220, tls/asio_tls_listener.cpp, ws/asio_ws_listener.cpp:151-162 | `process_term`이 4개 파일에서 구조적으로 동일(linger 저장→process_release_endpoint→drain_asio_listener_pending_accepts→own_t::process_term), 매크로 이름만 다름 | 공통 템플릿 헬퍼(asio 정책 헤더)로 추출, 트랜스포트별 콜백만 주입 — connecter 쪽 `prepare_asio_connecter_termination`과 동일 패턴으로 통일 | listener 4개 cpp + 신규/기존 asio 헤더 1개, ~60줄 이동 | 없음(내부 리팩터) | 없음 |
| 9 | 2 중복 | tcp/asio_tcp_listener.cpp:on_accept, ipc/…:on_accept, tls/asio_tls_listener.cpp:156-176(`on_tcp_accept`), ws/asio_ws_listener.cpp:on_accept | `_terminating` 체크 후 열린 소켓 닫고 return하는 블록이 4곳에 동일 문구로 반복 | #8과 같은 헬퍼에 accept-cancel 처리 공통화 | 상동 | 없음 | 없음 |
| 10 | 2 중복 | ws/ws_transport.cpp:21-40(`protocol_for_fd`, `ws_write_buffer_bytes`, `ws_read_message_max`) vs tls/wss_transport.cpp:23-42(동일 함수, `wss_` 접두) | wss_transport는 ws_transport를 재사용/합성하지 않고 전체를 병렬 재구현(같은 메서드 이름 open/is_open/close/async_read_some/read_some/async_write_some/async_writev/write_some/async_handshake 세트가 두 파일에 각각 존재, ~400/~475줄) | 최소: 3개 static 헬퍼를 공유 헤더로 추출. **확인 필요**(범위가 큼): beast socket 타입(plain vs ssl stream)만 다르므로 템플릿화 가능한지 별도 조사 필요 — 이번 job 범위를 넘는 구조 변경이라 별도 apply-job 권고 | 최소 안: ws/tls 2파일 + 공유 헤더 1개, ~40줄 / 최대 안(템플릿화): 2파일 전체, ~800줄 — **별도 검토 필요** | 없음(리팩터) | 없음~이득(코드 크기 감소, 런타임 동일) |
| 11 | 2 중복 | tls/wss_address.cpp:24-36 `wss_address_t::to_string` vs ws/ws_address.cpp:146-160 `ws_address_t::to_string` | prefix("ws://"/"wss://")만 다르고 나머지 로직(family 분기, host/port/path 조합) 동일 | wss가 protected 접근자로 ws 로직 재사용하는 protected helper(예: `format_url(prefix)`)로 통합 | 2파일, ~15줄 | 없음 | 없음 |
| 12 | 2 중복 | tcp/tcp_transport.cpp `async_writev`(등), ipc/ipc_transport.cpp `async_writev` 등 read_some/write_some/async_writev | stats 플래그(`tcp_stats_on`/`ipc_stats_on`) 이름과 소켓 타입만 다르고 구조 동일(윈도우/비윈도우 분기, writev 사용 여부 분기) 패턴이 tcp/ipc(+ws/tls 추정)에서 반복 | 템플릿 헬퍼로 socket 타입 파라미터화. **확인 필요**: ws/tls도 동일 패턴인지 라인 단위 대조(본 조사는 tcp/ipc만 diff 확인) | tcp/ipc/ws/tls transport 4개 cpp, 각 ~100줄 | 없음 | 위험(핫패스 함수 — 리팩터 후 벤치 필수, S-A/S-B 기준 재측정 요) |
| 13 | 확인 필요 | asio/asio_tcp_endpoint.hpp 3개 함수 | `asio_tcp_endpoint_to_sockaddr`는 3개 파일(tcp/tls/ws listener)에서만 쓰이고 ipc listener는 미사용 — ipc는 별도 주소체계(unix path)라 자연스러움. 문제 아님으로 판단되나 "정책" 이름과 실제 단일 프로토콜(tcp) 전용이라는 점에서 이름-개념 불일치 소지, 재명명 검토만 권고 | 재명명 검토(`policy`→`tcp_endpoint_codec` 등) | 헤더 1개 + 3 호출부 이름 변경 시 | 없음 | 없음 |

## 항목 수 요약
- 죽은 코드(1): 7건 (#1-7, ssl_context_helper 4건 + tcp.cpp 3건)
- 중복(2): 5건 (#8-12)
- 확인 필요만: 1건 (#13, 얕은 이름 소지)
- 얕은 모듈(3): 발견 없음 — tcp_transport.cpp/ipc_transport.cpp 함수 크기 점검 결과 250행 초과 함수 없음, connecter의 process_term/타이머 로직은 이미 `prepare_asio_connecter_termination`/`handle_asio_connecter_timer_event` 등 공유 헬퍼로 탈중복되어 있어 R2/이전 작업에서 처리된 것으로 보임(재작업 불요)
- 잘못된 소유(4): 발견 없음(범위 내)
- 이름-개념 불일치(5): #13만 약한 소지

## 적용 job 묶음 제안 (파일 안 겹침, 각 1.5h 이내)

1. **묶음 A — ssl_context_helper + tcp.cpp 죽은 코드 제거** (#1-7): `core/src/runtime/transports/tls/ssl_context_helper.{hpp,cpp}`, `core/src/runtime/transports/tcp/tcp.{hpp,cpp}`. 순서: #5→#6→#7(tcp.cpp, 서로 독립) 먼저 삭제 후 빌드 확인 → #1→#2→#3→#4(ssl_context_helper, from_pem 체인이므로 이 순서로 삭제) 후 재빌드. 두 그룹은 파일이 겹치지 않아 병렬 가능.
2. **묶음 B — listener 4종 process_term/on_accept 공통화** (#8, #9): `tcp/asio_tcp_listener.cpp`, `ipc/asio_ipc_listener.cpp`, `tls/asio_tls_listener.cpp`, `ws/asio_ws_listener.cpp` + 신규 `transports/asio/asio_listener_lifecycle.hpp`. 순서: 헬퍼 헤더 작성 → tcp 적용·테스트 → ipc/tls/ws 순차 적용(연결 관련 contract test test_endpoint_release로 회귀 확인).
3. **묶음 C — wss_address to_string 통합** (#11): `tls/wss_address.cpp`, `ws/ws_address.hpp`(protected 접근자 추가) — 소규모, 묶음 B와 파일 겹치지 않음.
4. **보류/별도 검토 — ws/wss transport 대형 중복(#10) 및 transport writev 템플릿화(#12)**: 반경이 크고 핫패스 성능 영향이 있어 이번 1.5h 단위에 넣지 않음. 별도 인벤토리/설계 job으로 분리 권고(특히 #12는 S-A/S-B 벤치 재측정 필수).
