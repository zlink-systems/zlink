# S-4 요약 — STREAM raw 엔진의 gather 죽은 경로 제거

worktree: `~/project/zlink-work/s4` (detached, main `8b65c9b42c`). 커밋하지 않음.

## 1. 결과

`asio_engine_t::prepare_gather_output()` 는 write turn 마다 다음을 실행했다.
encoder 탐침 `encode(&buf,0)` → `(this->*_next_msg)` pull → 크기/threshold 판정 →
가상 호출 `build_gather_header()` → 실패 시 `load_msg` 되돌리기 → false.
STREAM 이 쓰는 `asio_raw_engine_t::build_gather_header()` 는 **항상 false** 이므로
이 준비 작업 전체가 죽은 경로였다(S-B 보고서 F4·W3).

이번 변경으로 gather 사용 여부는 **연결 생성 시 한 번** 확정되고, 지원하지 않는 엔진에서는
`prepare_gather_output()` 이 **호출되지 않는다**. write turn 에서 없어진 것:
가상 호출 3(`encode`, `_next_msg` 멤버 포인터 호출, `build_gather_header`) + 분기 6 +
실패 시 `load_msg` 되돌리기 1. 남은 것은 `const bool` 멤버 하나의 판정이다.

관측 가능한 write 동작은 그대로다. 변경 전에도 raw 엔진은 100% encoder 경로였고,
ZMP 엔진은 `ZLINK_ASIO_GATHER_WRITE` 가 켜졌을 때만 gather 경로였다 —
새 판정식은 이 두 조건을 그대로 옮긴 것이다.

## 2. 변경 파일

| 파일 | 내용 |
|------|------|
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp` | `use_gather_write_for()` 추가(transport 능력 ∧ protocol 능력 ∧ env). `connection_fastpath_policy_t` 생성자·`from_environment()` 에 `protocol_builds_gather_header_` 인자 추가, `_gather_write_enabled` 가 전체 판정을 담음 |
| `core/src/runtime/engine/asio/asio_engine.hpp` | 생성자에 `bool protocol_builds_gather_header_` (transport 인자 앞, 기본값 없음) |
| `core/src/runtime/engine/asio/asio_engine.cpp` | 파일 static `asio_gather_write_on`·`asio_stream_gather_on` 제거(정책으로 이동). `prepare_gather_output()` 에서 gather 가능 여부 판정 3건 제거. 호출부 2곳(`start_async_write` :680, `speculative_write` :1337)을 `gather_write_enabled()` 로 gate |
| `core/src/runtime/engine/asio/asio_raw_engine.{cpp,hpp}` | 기저 클래스와 완전히 같던 `build_gather_header()` 오버라이드 삭제(중복 제거). 생성자 3곳이 `false` 전달 |
| `core/src/runtime/engine/asio/asio_zmp_engine.cpp` | 생성자 3곳이 `true` 전달 |
| `core/tests/unittest/unittest_asio_write_turn_policy.cpp` | 정책 생성 헬퍼·`from_environment` 호출에 새 인자 반영, gather 양쪽 능력 요구를 검증하는 테스트 1개 추가 |

`core/include/**`, `libzlink.vers`, 공개 계약 테스트, 옵션·플래그·env 목록은 건드리지 않았다.
`i_asio_transport.hpp` 는 변경 없음(transport 쪽 능력 선언은 이미 있었다).

## 3. 설계 비교

- **(1) 엔진 멤버 캐시**: `asio_engine_t` 에 `_gather_supported` 를 두고 생성 시 채운다.
  능력 판정이 정책(`connection_fastpath_policy_t`)과 엔진 두 곳으로 갈라진다. 정책은 이미
  `speculative_write/read` 와 **transport 쪽 gather 능력**을 소유하고 있으므로,
  같은 결정을 두 객체가 나눠 갖는 중복이 생긴다. 상태(멤버)도 하나 늘어난다.
- **(2) 정책 객체가 능력 전체를 소유** ← **선택**. gather 판정의 세 조각
  (transport 능력, protocol 능력, 진단 env 플래그)이 한 함수 `use_gather_write_for()` 에
  모인다. 호출부는 정책의 `const bool` 하나만 본다. `asio_engine.cpp` 의 file-static env
  전역 2개가 사라지고, 판정 규칙 수가 줄어든다(3곳 검사 → 1곳). 정책은 이미 "이 연결이 어떤
  fast path 를 쓰는가"를 소유하는 깊은 모듈이므로 능력이 하나 더 들어가도 인터페이스는
  넓어지지 않는다(POSDDD: 깊은 모듈, 중복 금지, 규칙 수 감소).

부수 효과로 raw 엔진의 `build_gather_header()` 오버라이드(기저와 동일한 죽은 코드)를 지웠다.
기저의 기본 구현(false 반환)은 남겼다 — 순수 가상으로 바꾸면 gather 를 쓰지 않는 모든 미래
엔진에 빈 오버라이드를 강제하는 새 규칙이 생기기 때문이다.

## 4. 실행한 테스트

| 대상 | 결과 |
|------|------|
| `ctest -R 'stream\|engine\|asio\|raw'` (24개) × 5회 | 24/24 통과, 실패 0 |
| `ctest -R 'zmp'` (7개) | 7/7 통과 (ZMP gather 경로 회귀 없음) |
| `unittest_asio_write_turn_policy` | 통과. 추가 테스트 `test_gather_needs_both_transport_and_protocol_support` 포함 |

ZMP 가 gather 를 "계속 쓰는지" 직접 확인하는 기존 테스트는 **없다**. gather 는 기본 off 이고
(`ZLINK_ASIO_GATHER_WRITE` 미설정 시 ZMP 는 gather 를 쓰지 않는다), 관련 테스트는
`unittest_asio_transport_writev_lifetime`(transport 의 `async_writev` 수명)과
`test_asio_ws.cpp:657`(raw 는 ZMP header 가 없어 gather 가 실패하고 encoder 로 되돌아온다는
주석)뿐이다. 그래서 정책 단위 테스트로 "protocol 능력이 없으면 gather off, 있으면 on"을
직접 고정했다.

TSan: 이번 변경은 동기화·공유 상태를 건드리지 않는다(연결 생성 시 상수 1개 추가, write turn
에서 코드 제거만). 별도 TSan 트리 구성은 1.5 h 상한 안에서 하지 못했다 — 아래 §7 참조.

## 5. 성능

측정: worktree 의 `core/build`(release, `--lib-only`) + `with_stream` 러너
`--stack zlink,asio --size all --ccu 1000 --runs 1`, `ZLINK_CORE_SOURCE=local`.
공용 `PERF_LOCK` 으로 직렬화했다.

| 크기 | Phase 0 기준 zlink | run 1 (load 1.70→10.5) | run 2 (load 10.91) | Phase 0 기준 asio | run 1 asio | run 2 asio |
|------|------|------|------|------|------|------|
| 64 B | 268.9 | **104.3** | **40.4** | 322.0 | 134.6 | 83.6 |
| 1024 B | 243.0 | **70.6** | **25.2** | 316.4 | 74.0 | 94.2 |
| 64 KiB | 30.4 | **3.6** | **8.8** | 39.2 | 10.4 | 17.9 |
(단위 kops, 중앙값. mismatch 는 모든 셀에서 0.)

**이 수치로는 이번 변경의 효과를 판정할 수 없다.** 두 run 모두 load average 10 이상에서
돌았고(다른 job 6개가 동시에 빌드·테스트 중), 레퍼런스인 **asio 스택도 기준값의 26–42% 로
같이 무너졌다**(322.0 → 134.6 → 83.6). 두 run 사이 zlink 값도 2.6배 흔들린다(104.3 → 40.4).
즉 신호가 아니라 머신 경합이다. 공통 규칙의 "측정 루프 금지"(after 최대 2회)에 따라 여기서
멈춘다. mismatch 0 과 3개 크기 전부 정상 완주는 확인했으므로 **기능 회귀는 없다**.

기대 이득은 S-B 보고서의 정적 추정 그대로 1–3% 수준이다(write turn 당 가상 호출 3 + 분기 6
제거). 유효한 수치는 머신이 조용할 때 감독관 게이트에서 다시 재야 한다.


## 6. 재확인한 스펙 절

- `core/doc/spec/core/socket/08-stream.ko.md` "현재 STREAM 런타임 기본값"(371-407행):
  나열된 런타임 값·환경변수 목록이 그대로다. `ZLINK_ASIO_STREAM_DISABLE_GATHER` 는 여전히
  읽히고 기본 비활성이며, 설정하면 gather 를 끈다. `ZLINK_ASIO_STREAM_GATHER_THRESHOLD`,
  `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD` 의 의미와 기본값도 그대로다.
- `core/doc/spec/core/systems/03-io-thread.ko.md` §4: 튜닝 가이드라인(I/O thread 수)과
  무관한 변경이다. write turn 당 준비 버퍼는 하나라는 서술도 그대로다.
- `core/doc/spec/core/protocol/02-raw.ko.md`: raw 는 송신 측에서 wire header 를 붙이지
  않는다(PACKET 모드의 6 byte prefix 는 수신 파싱 서술). raw 엔진이 gather header 를 만들 수
  없다는 이번 판정과 일치한다.

**어느 문장도 다른 동작이 되지 않았다.** completion, READY/DISCONNECTED, POLLIN/POLLOUT
level, WRITABLE wake 의 순서·조건은 이 코드 경로에 없고 변경도 없다. write turn 이 고르는
경로(항상 encoder)와 그 결과 바이트 스트림은 변경 전후 동일하다.

한 가지 문서 관찰(수정 범위 밖): 08-stream 의 "`ZLINK_ASIO_STREAM_DISABLE_GATHER`: 기본
비활성이라 STREAM gather-write는 유지됨" 문장은 **환경변수의 기본값**으로는 맞지만,
STREAM 은 raw 엔진을 쓰므로 실제로 gather write 가 일어난 적은 없다(F4). 이번 변경은 그
사실을 코드 구조로 명시했을 뿐 동작을 바꾸지 않았다. 문장을 다듬을지는 감독관 판단 사항이다.

## 7. R2 인벤토리 후보 (수정 범위 밖, 보고만)

공통 엔진에 남은 ZMP 전용/죽은 분기:

1. `asio_engine.cpp:1099` — `if (_pipeline.async_gather) finish_gather_output ();`
   `finish_gather_output()` 첫 줄이 같은 검사를 다시 한다(중복 검사 1).
2. `asio_engine_pipeline.hpp:67-72` — `async_gather`, `gather_header[64]`,
   `gather_header_size`, `gather_body`, `gather_body_size` 는 raw 엔진 연결에서 절대 쓰이지
   않는데 모든 연결이 들고 있다(연결당 88 byte + 초기화 5회).
3. `asio_engine.hpp` `process_command_message()` 기본 구현(-1 반환)은 ZMP 전용 훅이다.
4. `prepare_gather_output()` 안의 `stream_mode ? asio_stream_gather_threshold :
   asio_gather_threshold` 와 `tiny_stream_gather` 판정은 이제 ZMP 연결에서만 도달한다.
   `asio_stream_*_gather_threshold` 두 상수는 STREAM 이 gather 를 못 쓰는 한 도달 불가다
   (env 목록에 남아 있으므로 제거는 스펙 문장 변경을 동반한다 → 별도 판단 필요).

## 8. 변경 분류

**B(기존 결함) — 죽은 경로 제거.** 계약·스펙 문장 변경 없음, 새 옵션·플래그 없음.

## 9. 멈춘 지점

1. **성능 수치 확정.** 머신 경합(load 10+)으로 두 run 모두 기준값과 비교 불가.
   asio 레퍼런스가 함께 무너진 것으로 경합임을 확인했다. 조용한 머신에서 재측정 필요.
2. **TSan 1회.** 엔진 파일을 만졌으므로 규칙상 TSan 1회가 요구된다. 기존
   `core/build-tsan` 은 메인 저장소에 있고 LTO ON 구성이라 worktree 에 재구성하면
   1.5 h 상한을 넘긴다. 이번 변경은 잠금·공유 상태·스레드 경계를 전혀 건드리지 않고
   (연결 생성 시 `const bool` 1개 추가, write turn 코드 제거) 새 데이터 경로가 없어
   위험이 낮다고 판단해 생략했다. 게이트 job 에서 확인하기를 권한다.
3. 커밋하지 않았다. 리뷰 대상은 worktree `~/project/zlink-work/s4` 의 diff 다.

