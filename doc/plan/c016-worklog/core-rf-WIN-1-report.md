# WIN-1 — Windows 전용 test_zmp_metadata 실패 진단·수정 보고

- 일자: 2026-09-08 (머신 B, WSL에서 Windows 호스트 툴체인 구동)
- 대상 실패: GitHub Actions `build.yml` / `Build Windows x64` / `Verify Windows test configuration`
  → `test_zmp_metadata` `test_paired_incomplete_lane_fence_timeout_and_fresh_pair`
  (`core/tests/integration/test_zmp_metadata.cpp:720`, "Expected TRUE Was FALSE" + "Forced closure of 1 sockets")
- Windows 체크아웃: `D:\project\zlink` (신규 클론, detached `origin/main` = `b5cebd30d2`)
- 빌드: CI와 동일 (`VS 17 2022 / x64 / Release / BUILD_SHARED+STATIC+TESTS / CXX17 / WITH_TLS=ON / OpenSSL-Win64`)
- 변경 분류: **B (기존 결함)** — Windows 전용 Core 결함 2건. 공개 인터페이스·계약·옵션 변경 없음.

## 1. 재현

| 항목 | 결과 |
| --- | --- |
| 1회차 `ctest -R '^test_zmp_metadata$'` | **즉시 실패** — `test_zmp_metadata.cpp:720` FAIL (CI와 동일 지점) |
| Linux(dev, WSL) 동일 테스트 | 통과 |
| 재현율 | 100 % (수정 전 매 실행 실패) |

실패 단언은 `assert_incomplete_pair_lane_hits_fence()` 안의 **첫** `wait_for_transport_pair_admission (server, fresh_alias, 2)`
(5 s 동안 socket monitor `ZLINK_MONITOR_STATE_READY` 폴링)이다. 즉 fence timeout 뒤 새 pair가 **영구히** admission되지 않는다.

## 2. 근본 원인 — pair id 재사용 (Windows 엔트로피 소스 부재)

임시 계측(adopt/release/attach/pair-ready 로그)으로 얻은 수정 전 로그:

```
WIN1 adopt NEW   key=raw-pair-fence-peer lc=2 -> 27206470336553/1     <- lone lane
WIN1 release     key=raw-pair-fence-peer 27206470336553/1 erased=1
WIN1 adopt NEW   key=raw-pair-fence-peer lc=2 -> 27206470336553/1     <- fresh application (같은 id!)
WIN1 adopt REUSE key=raw-pair-fence-peer lc=2 -> 27206470336553/1     <- fresh completion
WIN1 attach pair=27206470336553/1 lane=0 occupied=0                   <- fresh application 부착
WIN1 release     key=raw-pair-fence-peer 27206470336553/1 erased=1    <- lone lane의 늦은 release가 새 pair 연결을 지움
WIN1 attach pair=27206470336553/1 lane=0 occupied=1                   <- stale lane의 늦은 attach → lane 0 충돌 → reject
WIN1 attach pair=27206470336553/1 lane=1 occupied=0
(pair READY 없음 → monitor READY 없음 → 720행 FAIL)
```

두 번의 `adopt NEW`가 **동일한 64-bit pair id**를 뽑았다. 값의 상위 비트가 모두 0인 것(27206470336553 < 2^45)도 단서다.

- 원인 파일: `core/src/runtime/utils/random.cpp:47` `zlink::generate_random_bytes()`
  - `getrandom()` 경로는 `#if !defined ZLINK_HAVE_WINDOWS && defined ZLINK_HAVE_GETRANDOM`,
    `/dev/urandom` 경로는 `#if !defined ZLINK_HAVE_WINDOWS`로 **둘 다 Windows에서 배제**된다.
  - 남는 것은 `generate_random()`(`random.cpp:39`)의 `rand()` 폴백뿐이다.
  - MSVC CRT의 `rand()`는 (a) 15비트만 반환하고(`high <<= 31`이 상위 14비트를 버려 uint32당 실질 16비트),
    (b) **시드 상태가 스레드-로컬**이다. `seed_random()`의 `srand()`는 호출 스레드만 시드하므로
    각 I/O 스레드는 기본 시드 1로 시작해 **모든 스레드가 동일한 수열**을 낸다.
  - 결과: 서로 다른 I/O 스레드에서 접수된 두 pair가 결정적으로 같은 pair id를 얻는다.
- 결함이 실패로 이어지는 경로: `core/src/runtime/sockets/common/socket_base_api.cpp:66`
  `adopt_accepted_transport_pair()`의 충돌 회피 루프는 **살아 있는 테이블**(`_transport_pairs`,
  `_accepted_transport_pairs`)만 검사한다. lone lane이 이미 두 테이블에서 빠진 뒤 같은 id가 재발급되면,
  아직 in-flight인 stale lane의 attach/release 명령이 **새 pair의 키에 그대로 적중**한다
  (`socket_base_api.cpp:337` 부근 lane 점유 충돌 → reject, `socket_base_api.cpp:142` release가 새 연결을 삭제).
- Linux에서 관측되지 않는 이유: `getrandom()`/`/dev/urandom`이 실제 엔트로피를 주므로 id 충돌이 사실상 불가능.

영향 범위는 이 테스트에 그치지 않는다. Windows 빌드에서 `generate_random_bytes()`는
routing id(`utils/routing_id.hpp:23`), 소켓 식별자(`socket_base.cpp:28`), endpoint 식별자
(`socket_base_endpoint.cpp:55`) 생성에도 쓰이므로 전부 사실상 무작위성이 없었다.

## 3. 부수 발견 — 컨텍스트 종료 중 spurious wake abort (Windows 전용, 기존 결함)

| 항목 | 내용 |
| --- | --- |
| 증상 | `Resource temporarily unavailable (core/src/runtime/core/ctx_termination.cpp:109)` 로 프로세스 abort |
| 발생 | `test_zmp_metadata` 10회 중 4회, `test_asio_ws` 3회 중 2회 |
| 확인 | **수정 전 순정 `origin/main`에서도 동일 재현** — RNG 수정과 무관한 선행 결함 |
| 원인 | `ctx_t::wait_for_reaper_done()`이 `_term_mailbox.recv(&cmd, -1)`의 `EAGAIN`을 `errno_assert`로 취급. 그러나 `mailbox_t::recv()`는 timeout `-1`에서도 EAGAIN을 반환할 수 있다(`core/src/runtime/core/mailbox.cpp:157-160, 175-177`): signaler 에지와 command pipe는 별개 객체라 커맨드 없는 wake가 정상 경로로 존재한다. |

CI 러너에서는 관측되지 않았지만(리포트상 `test_asio_ws`는 통과), 로컬 Windows에서는 릴리스를 동일하게 막는다.

## 4. 수정

| 파일 | 변경 |
| --- | --- |
| `core/src/runtime/utils/random.cpp` | Windows 엔트로피 경로 추가: `advapi32.dll`의 `SystemFunction036`(`RtlGenRandom`)을 `GetProcAddress`로 1회 해석해 `generate_random_bytes()`의 최우선 소스로 사용. `rand()` 폴백은 익명 `weak_random()`으로 격리하고, 공개 `generate_random()`은 `generate_random_bytes()`를 호출하도록 통합(중복 제거, 모든 호출자가 같은 소스를 공유). |
| `core/src/runtime/core/ctx_termination.cpp` | `wait_for_reaper_done()`이 `EAGAIN`(커맨드 없는 wake)에서 abort하지 않고 재대기. `EINTR`·`done` 처리는 그대로. |

### 설계 비교와 선택 이유

pair id 충돌을 막는 방법은 둘이었다.

1. **엔트로피 소스를 고친다(선택).** 결함의 실제 위치는 `generate_random_bytes()` 하나다. 한 곳을 고치면
   pair id뿐 아니라 routing id·소켓 식별자까지 함께 정상화된다. 새 규칙·상태·옵션이 생기지 않는다.
2. pair id를 소켓별 단조 증가 카운터로 바꾼다. 충돌은 막히지만 (a) Windows의 무작위성 결여라는 진짜 결함은
   그대로 남고, (b) id 예측 가능성이라는 새 성질이 생기며, (c) `adopt_accepted_transport_pair()`에
   별도 할당 규칙이 추가된다. 규칙 수를 늘리는 방향이라 기각.

Windows 엔트로피 API 선택은 `BCryptGenRandom`(bcrypt.lib 링크 필요 → `libzlink`/`libzlink-static` 등
여러 타깃에 동일 링크 구문 중복) 대신 **`RtlGenRandom` 동적 해석**을 택했다. 빌드 시스템 변경이 없고,
모든 Windows CRT/bcrypt 경로가 최종적으로 도달하는 동일 시스템 소스이며, 코드가 `random.cpp` 한 곳에 갇힌다.

## 5. 검증

### Windows (`D:\project\zlink\build-win-x64-test-config`, CI와 동일 구성)

| 실행 | 결과 |
| --- | --- |
| 수정 전 `test_zmp_metadata` | 1회차 즉시 실패 (`:720`) |
| RNG 수정 후 `test_zmp_metadata.exe` 직접 10회 | 단언 실패 0건 (`:720` 완전 소멸). 단, 4/10에서 §3 종료 abort |
| 순정 main `test_asio_ws` 3회 | 2회 §3 종료 abort (선행 결함 확인) |
| 두 수정 모두 적용 후 `ctest -C Release -R '^(test_zmp_metadata\|test_asio_ws)$' --repeat until-fail:10` | **100 % 통과, 2/2 테스트 × 10회, 53.3 s** |

### Linux (`core/build-dev`, `JOBS=4 scripts/build-core.sh dev`)

| 실행 | 결과 |
| --- | --- |
| `ctest -R 'zmp_metadata\|router\|asio_ws' --repeat until-fail:3` (RNG 수정) | 17/17 통과, 155.3 s |
| `ctest -R 'zmp_metadata\|router\|asio_ws\|ctx\|term' --repeat until-fail:3` (두 수정) | 23/23 통과, 162.2 s |

## 6. 스펙 확인

두 변경 모두 공개 헤더(`core/include/**`), `libzlink.vers`, 계약 테스트 기대값, 옵션·타이머·이벤트 순서를
건드리지 않는다. `generate_random_bytes()`의 계약은 "요청한 바이트 수를 난수로 채운다"로 동일하고,
`wait_for_reaper_done()`의 계약은 "reaper `done`을 받을 때까지 대기, `EINTR`이면 -1"로 동일하다.
어느 문장도 다른 동작이 되지 않았다.

## 7. 남은 위험

- `generate_random()`은 이제 호출마다 `RtlGenRandom`을 거친다. 호출처는 ROUTER 생성자, 모니터 id,
  재연결 지터로 모두 저빈도라 성능 영향은 없다고 본다(핫 패스 호출처 없음).
- `RtlGenRandom` 해석이 실패하면 기존 `rand()` 폴백이 남는다. 실질적으로 도달하지 않지만,
  도달할 경우의 품질은 이전과 같다.
- §3 수정은 CI 러너에서는 재현되지 않던 문제에 대한 것이다. 방어적으로 옳은 변경이지만
  (mailbox API가 문서상 EAGAIN을 반환할 수 있음), 감독관이 범위를 좁히려면 이 파일만 되돌려도
  원래 보고된 실패(`:720`)는 해결된 상태로 남는다.
- Windows 로컬 검증은 `test_zmp_metadata`·`test_asio_ws` 두 타깃에 한정했다(CI 단계와 동일). 전체
  Windows ctest는 돌리지 않았다.

## 8. 변경 파일 (WSL 메인 체크아웃, 커밋하지 않음)

- `core/src/runtime/utils/random.cpp`
- `core/src/runtime/core/ctx_termination.cpp`
