# Protocol — RAW 스펙-구현 gap 감사

> 감사 도구: codex (gpt-5, 정적 대조) · 2026-08-24
> 대조 범위: RAW encoder/decoder, ASIO RAW engine, STREAM packet-dispatch framing, packet wire primitive 및 정적 test 표본

판정: **검증 항목 14개 전부 일치**. 구현/문서는 수정하지 않았고, 실행 테스트 없이 정적 대조만 수행했다.

## 대조 완료 계약군

- STREAM 전용 RAW 선택과 tcp/ipc/tls/ws/wss listener의 `asio_raw_engine_t` 선택: 일치
- 순수 RAW의 무프레이밍 송신·수신, zlink 계층 handshake 생략: 일치
- packet handler 모드 전환, `header_size(2B)` + `body_size(4B)` Big Endian byte 배치: 일치
- packet fragment 결합·연속 packet 분리, header/body 별도 `zlink_msg_t` callback 전달: 일치
- RAW 연결의 monitor 준비·해제 이벤트와 0 byte 제어 payload의 dispatch 제외: 일치
- 내부 4 byte `uint32` routing id와 송수신 2-frame 배치: 일치

## Gap 목록

발견된 gap 없음.

RAW encoder는 현재 message의 `data()`와 `size()`만 다음 write 단계로 넘기고, RAW engine의 gather header도 크기 `0`으로 설정한다. decoder는 입력 span 전체를 `zlink_msg_t`로 만들어 소비량을 동일한 `size_`로 기록한다. 따라서 순수 RAW 경로에는 zlink wire header나 payload 변환이 없다. `raw_encoder.cpp:16-20`, `raw_decoder.cpp:38-81`, `asio_raw_engine.cpp:114-123`.

packet handler 경로는 pipe별 6 byte prefix를 누적한 뒤 `get_uint16(prefix)` 및 `get_uint32(prefix + 2)`로 길이를 읽고, 완료된 두 buffer를 별도 message로 이동해 callback에 전달한다. wire primitive는 최상위 byte부터 조합하므로 문서의 Big Endian 배치와 일치한다. `stream.cpp:813-938`, `wire.hpp:29-46`. 정적 integration test도 split prefix, 같은 read에 결합된 두 packet, 빈 header/body 및 빈 header+body record를 각각 대조한다. `test_stream_socket.cpp:1999-2074`.

## 요확인

- 없음. `ZLINK_OPT_MAXMSGSIZE` 초과 및 packet malformed 처리, handler 등록/수명·오류 규약은 대상 문서가 Socket — STREAM에 명시적으로 위임했으므로 이 감사의 gap으로 계상하지 않았다.
