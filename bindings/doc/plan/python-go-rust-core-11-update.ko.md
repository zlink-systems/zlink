# Python·Go·Rust bindings Core 0.9.0 최신화 공통 계획

> 대상 독자는 세 bindings의 Core 입력을 준비하고 각 언어 작업의 시작·완료를 판정하는 구현 담당자와
> reviewer다. 이 문서는 “어떤 Core candidate를 공통 입력으로 사용하고, Python·Go·Rust 작업을 어떻게
> 서로 독립적으로 진행하는가?”에 답한다.

## 1. 현재 판단

Python, Go, Rust bindings는 현재 Core 0.9.0 공개 계약과 맞지 않는다. Repository `VERSION`은 `11.1.0`이지만
세 bindings의 Linux x86_64 runtime은 `10.6.0`이고 Linux aarch64 runtime은 major 9 SONAME을 사용한다.
Go와 Rust의 복사된 header에는 `zlink/service/`가 있으며, 세 언어의 공개 API와 sample에도 MeshNode,
Spot과 Actor가 남아 있다.

Core 0.9.0은 raw socket runtime만 제공하고 service runtime은 Framework가 소유한다. 세 bindings는 raw C API만
투영하고 service C ABI, wrapper와 호환 alias를 제공하지 않아야 한다. Native library 교체, source test,
package 검증, clean consumer와 지원 platform 검증을 모두 통과해야 최신화가 완료된다.

이 계획은 **공통 준비 미완료, 언어별 구현 미착수** 상태다. 2026-08-03에 확인한 `core/build` runtime은
`11.1.0`을 반환하지만 `core/include/zlink/socket/api.h`보다 오래되었다. 이 runtime은 package 입력이나 완료
증거로 사용할 수 없다. 확인한 revision, 파일, 명령과 hash는
[기준선 조사 log](log/common/2026-08-03-baseline.ko.md)에 기록했다.

## 2. 문서 소유권과 독립 실행

공통 계획은 Core candidate, raw symbol inventory, Go·Rust parity 통합 gate와 최종 판정만 소유한다. 반환과
error 계약은 공통 bindings spec이 소유하고, parity inventory는 두 언어가 그 계약을 같은 의미로 구현했는지
검증한다. 언어별 source, package, consumer, sample과 platform 결과는 다음 문서가 각각 소유한다.

| 작업 | 실행 문서 | 다른 언어와의 의존성 |
|------|-----------|----------------------|
| 반환과 error 정식 계약 | [공통 bindings spec](../spec/README.ko.md)의 `오류 처리 정책` 절 | 구현이 따라야 하는 정본 |
| Python | [Python 실행 계획](python-core-11-update.ko.md) | 공통 gate 이후 없음 |
| Go | [Go 실행 계획](go-core-11-update.ko.md) | 공통 gate와 Go module 결정 이후 없음 |
| Rust | [Rust 실행 계획](rust-core-11-update.ko.md) | 공통 gate 이후 없음 |
| Go·Rust 계약 비교 | [Go·Rust parity inventory](go-rust-return-parity.ko.md) | 검증 목록과 언어별 evidence를 소유 |
| Go·Rust 통합 상태 | 이 공통 계획의 PGR-COMMON-03 | 두 언어 결과가 모두 있어야 판정 가능 |

공통 시작 gate인 PGR-COMMON-01, PGR-COMMON-02와 PGR-COMMON-04가 통과하면 세 언어를 병렬 또는 원하는
순서로 진행할 수 있다. 한 언어의 실패는 공통 Core
candidate가 원인이 아닌 한 다른 언어의 source 작업을 막지 않는다. 통합 완료 판정은 세 언어 문서와 parity
inventory가 모두 완료된 뒤 수행한다.

## 3. 계약과 책임 경계

Core 0.9.0 계약은 다음 자료가 소유한다.

1. `core/doc/spec/core/`의 정식 spec
2. `core/include/zlink.h`와 이 header가 포함하는 domain header
3. Core contract test와 설치 package가 제공하는 공개 API 목록

언어별 표현은 [공통 bindings spec](../spec/README.ko.md)과 언어별 bindings spec을 따른다. 다른 언어 구현은
이름과 언어 관례를 비교할 때만 참고하며 계약 근거로 사용하지 않는다.

Core 0.9.0 binding이 제공하는 기능은 context, message, raw socket, transport, eventing과 utility다. MeshNode,
ChannelName membership, ready batch, claim, Spot, Actor, transfer와 bound STREAM session은 Framework 책임이다.
Private FFI, deprecated API, sample helper와 package 내부 symbol로 service 기능을 유지하지 않는다.

## 4. 공통 Core candidate 승인

### 4.1 승인 입력

Binding package는 `scripts/local-package/core/build-wsl.sh`가 만든 Core install prefix를 입력으로 사용한다.
Core 변경 파일을 봉인한 manifest, 그 manifest를 승인한 독립 review 결과, 승인된 입력으로 install prefix를
만들고 clean C consumer까지 통과한 package 결과가 순서대로 같은 candidate를 가리켜야 한다.

| Artifact가 증명하는 내용 | 생성·검증 주체 | 필수 identity와 상태 |
|--------------------------|----------------|----------------------|
| Core 변경 파일과 base revision이 고정되어 있음 | `scripts/v11/create-ledger-candidate.mjs`, `scripts/local-package/core/verify-candidate.mjs` | Candidate `ledgerId=V11-M3-CORE-VERIFY` |
| 독립 reviewer가 바로 그 candidate를 승인함 | V11-R2 review와 `verify-candidate.mjs` | `ledgerId=V11-R2`, `status=passed`, `details.approvedCandidateManifestSha256=<candidate file SHA-256>` |
| 승인된 candidate로 install·clean C consumer가 통과함 | `scripts/local-package/core/build-wsl.sh` | `ledgerId=V11-M3-CORE-PKG`, `status=pass` |
| Install prefix의 파일과 provenance가 package 결과와 같음 | Core package evidence의 `output`과 install prefix의 provenance manifest | `output.prefix`, `output.provenanceManifest`, `output.provenanceSha256` 정확히 일치 |

Candidate file SHA-256은 package evidence의 `candidate.manifestSha256`, review 정보의
`approval.candidateManifestSha256`과도 같아야 한다. Package provenance의 candidate `baseRevision`,
`manifestSha256`과 `aggregateSha256`은 package evidence와 같아야 한다. Clean C consumer는 package prefix
밖의 header와 library path 없이 compile, link, load와 runtime version을 검증해야 한다.

기본 evidence 위치는 `.artifacts/v11/evidence/`다. 실행 시 선택한 절대 경로를 공통 log에 기록하며 “가장
최근 파일”을 자동 선택하지 않는다. 2026-08-01의 `core-package-20260801.json`은 구조 예시일 뿐, 현재
candidate와 hash가 다르면 재사용하지 않는다.

### 4.2 Core 입력과 binding source manifest

`scripts/local-package/bindings-candidate/create-manifest.sh`는 repository `core/build`을 직접 읽지 않는다.
다음 absolute path를 명시적으로 입력받는다.

- Core package evidence JSON
- Core package evidence의 `output.prefix`가 가리키는 install prefix
- `output.provenanceManifest`가 가리키는 provenance manifest와 `output.provenanceSha256`
- Platform별 runtime과 public header root
- 언어 이름과 binding source manifest 출력 경로

언어별 manifest는 Core candidate identity, package provenance hash, public header hash, platform별 runtime
SHA-256, SONAME 또는 DLL identity, `zlink_version()`과 exported `zlink_*` symbol inventory를 기록한다. 또한
해당 binding의 source, test, sample, perf, package metadata와 build script를 대상 파일로 선언하고 다음 값을
봉인한다.

- Binding candidate의 base revision
- 대상 파일의 path, content SHA-256, mode와 base hash
- 대상 파일 목록으로 계산한 aggregate SHA-256
- 공통 package tooling처럼 candidate 밖에서 읽는 direct input의 path와 SHA-256

Source test와 package build는 live worktree가 아니라 이 manifest로 materialize한 격리 snapshot에서 실행한다.
구현 중에는 승인 draft의 path와 SHA-256을 작업 log에 기록한다. 구현과 contract test 뒤 정식 문서를 갱신한
다음 만드는 최종 manifest의 direct input에는 공통 bindings spec과 해당 언어 spec의 path와 SHA-256을
포함한다. 정식 문서가 바뀌면 code가 같더라도 manifest를 다시 만들고 필수 gate를 재실행한다.
Package artifact, clean consumer evidence와 독립 review는 모두 같은 binding source manifest의 file SHA-256과
aggregate SHA-256을 기록한다. Core evidence, platform artifact, binding source 또는 build script가 바뀌면
manifest 재사용을 거부하고 다시 review한다.

### 4.3 Raw symbol allowlist

Service symbol 제거는 prefix 몇 개를 금지하는 방식으로 검사하지 않는다. 기존
`core/tests/contract/check_public_surface.py`를 확장해 정식 Core spec, 설치 header와 package runtime export가
정확히 일치하는 machine-readable 목록과 hash를 만들고 Core package evidence에 기록한다. Bindings gate는
이 결과를 입력받아 FFI와 package payload를 비교한다. Package runtime에 허용 목록 밖 symbol이 있거나 header
함수가 runtime에 없으면 실패한다.

이 검사는 `zlink_set_mesh_node_option`, `zlink_instance_spot_*`처럼 이름이 기존 금지 prefix에 포함되지 않는
service symbol도 차단한다. 설치 header에 `zlink/service/`가 있거나 root `zlink.h`가 service header를
include해도 실패한다.

## 5. Platform provenance

Core candidate identity는 platform이 달라도 같은 `baseRevision`, candidate manifest hash와 aggregate hash를
사용한다. Runtime SHA-256, SONAME 또는 DLL identity와 loader 결과는 platform별 evidence로 따로 기록한다.
서로 다른 platform runtime에 같은 SHA-256을 요구하지 않는다.

WSL package gate는 Linux host 결과만 증명한다. 각 언어 실행 문서는 실제로 지원한다고 명시한 platform마다
다음 절차를 소유한다.

1. 승인 candidate를 격리 snapshot으로 materialize해 해당 platform Core artifact를 만든다.
2. Platform build evidence에 Core candidate manifest hash와 aggregate hash, toolchain, build configuration,
   contract test 결과, runtime hash와 public header hash를 기록한다.
3. 언어의 package 형식에 맞춰 해당 platform payload를 포함하고 이전 major runtime을 제거한다. Python은
   platform별 wheel을 만들고, Go와 Rust는 같은 module archive 또는 crate에 지원 platform payload를 모두 넣는다.
4. Binding source manifest가 해당 platform evidence의 exact path와 SHA-256을 입력으로 봉인하게 한다.
5. 해당 platform의 native consumer에서 package 내부 runtime load와 version을 확인한다.

현재 tooling에 생성 경로가 없는 platform은 release 완료 조건에 포함할 수 없다. 그 platform을 지원하려면
별도 Core artifact 생성, binding loader/linker와 native consumer를 먼저 구현한다. 지원을 제외할 때는
언어별 spec과 guide의 platform 표를 구현 완료 후 실제 상태에 맞춘다.

## 6. Go·Rust return-based parity

반환과 error 의미의 정본은 [공통 bindings spec](../spec/README.ko.md)의 `오류 처리 정책` 절이다.
[Go·Rust parity inventory](go-rust-return-parity.ko.md)는 이번 작업에서 두 언어를 비교하기 위한 검증 요약이며
새 계약을 정의하지 않는다.

Caller-provided receive의 no-data는 Go에서 `(false, nil)`, Rust에서 `Ok(false)`인 정상 결과다. Submit
backpressure는 공통 오류 처리 정책에서 함수군별 error로 정의하지만, 현재 구현은 Go `false, nil`과 Rust
`Ok(false)`를 반환하고 비동기 완료 정책도 boolean 반환을 허용한다. 따라서 exact submit signature는 아직
정식 계약 사이에서 일치하지 않는다.

이전에 작성된 Go·Rust submit 반환 설계 후보는 Go `error`와 Rust `Result<(), SubmitError>`를 비교했지만
공통 승인을 받지 못해 삭제했다. 현재 구현과 정식 spec의 반환 차이는 parity inventory에 기록하며, 이 문서의
내용을 구현 지시로 사용하지 않는다. 정식 spec과 public signature를 바꾸려면 별도 공통 승인, 구현과
contract test, implementation gap 기록이 모두 필요하다.

Parity inventory는 함수군 error, 입력 검증, no-data, `code`, `internal_errno`와 ownership뿐 아니라 submit
signature와 언어별 public naming도 판정한다. Python·Rust part 접근 이름의 현재 차이는 inventory에 기록하며,
승인되지 않은 설계 후보를 구현 기준으로 사용하지 않는다. 구현과 contract test가 통과하고 별도 승인을 받은
뒤에만 정식 spec을 반영한다.

## 7. 공통 준비 작업

### 구현 단계의 성능 경계

이번 최신화는 성능 개선 수치나 전체 benchmark 실행을 완료 조건으로 삼지 않는다. 실제 throughput·latency
개선과 언어 간 수치 비교는 Core 0.9.0 최신화가 끝난 뒤 언어별 후속 계획에서 진행한다.

다만 새 wrapper와 FFI 연결이 불필요한 allocation, payload copy 또는 lock contention을 hot path에 추가하면
후속 최적화의 기준선 자체가 나빠진다. 각 언어는 `tests/hot-path-cost-inventory.json`을 만들고 다음 내용을
기록한다.

- Public API 호출부터 Core 호출까지 실행 경로를 추적하고, 각 단계에서 buffer를 누가 소유하는지 기록한다.
- Copy가 발생하는 위치를 기록하고, 공개 계약이나 C ABI가 요구하는 copy인지 구현 편의로 추가된 copy인지
  분류한다.
- 반복 호출 중 collection, wrapper, closure, task 또는 native 임시 buffer를 만드는 위치를 기록한다.
- Lock, GIL, mutex, channel과 atomic 연산이 보호하는 상태와 적용 범위를 기록한다.
- 각 비용을 `required`, `removed`, `unclassified`로 분류하고 source symbol, 이유와 이를 검사하는 test 이름을
  연결한다.

Inventory 검사에서 `unclassified`가 0건이어야 한다. `required`에는 공개 계약, C ABI, memory safety 또는
callback 실행 의미 때문에 제거할 수 없는 이유가 있어야 한다. 이유가 없거나 범위가 실제 보호 대상보다 넓은
항목은 통과하지 않는다. Python은 `tests/test_optimization_guard.py`, Go는
`internal/native/optimization_guard_test.go`, Rust는 `tests/optimization_guard_tests.rs`에서 inventory와
source를 대조한다. 세 검사 모두 종료 코드 0이어야 한다. 명령, inventory SHA-256, 분류별 건수와 test 결과는
`bindings/doc/plan/log/<language>/<date>-hot-path-review.ko.md`에 기록한다.

성능을 위해 ownership 계약을 약화하거나 borrowed buffer의 수명을 호출자에게 떠넘기지 않는다. 새 public
zero-copy API, raw handle, private FFI 우회와 perf 전용 fast path도 추가하지 않는다.

Perf 검증은 runner smoke까지만 수행한다. Smoke는 binding source manifest로 materialize한 snapshot과 승인
package payload를 사용한다. Runner가 repository `core/build`, 전역 library path 또는 다른 native payload를
선택하면 실패한다. 실행 log에는 load한 runtime의 absolute path와 SHA-256을 기록하고 platform evidence와
비교한다.

각 언어 runner에 `--smoke` mode를 추가한다. 이 mode는 공식 report directory와 baseline inventory에 파일을
만들지 않고 표준 출력으로만 결과를 제공해야 한다. 기존 report 생성 경로를 임시 directory로 바꾸는 것만으로
대체하지 않는다. 실행 뒤 smoke report가 공식 또는 임시 결과 directory에 남아 있으면 실패한다.

각 언어는 single `PAIR/inproc/64B`와 multi `MULTI_DEALER_ROUTER/tcp/64B/1 client`를 1초, 1회 실행한다. 이
조건은 runner lifecycle을 확인하기 위한 smoke 전용이며 공식 성능 결과로 저장하거나 비교하지 않는다.
Runner evidence는 ready 통과, active phase 집계, 필수 metric 집합과 exit code를 기록한다. 필수 metric은
`throughput`, `bandwidth`, `latency`, `latency_p95`, `latency_p99`이며 각 값은 유한한 0 이상의 수여야 한다.
Single과 multi가 각각 이 metric의 `RESULT,current` 행을 모두 출력하고 exit code 0으로 끝나야 통과한다. 전체
pattern·transport matrix, 반복 통계, baseline 비교와 성능 threshold 판정은 이번 완료 조건에서 제외한다.
실행 명령, runtime hash, lifecycle 판정, 표준 출력의 `RESULT` 행과 exit code는
`bindings/doc/plan/log/<language>/<date>-perf-smoke.ko.md`에 기록한다.

### 구현 후 품질 gate

각 언어는 구현 항목과 구현 중 test가 모두 통과한 뒤 정식 문서와 package 작업으로 넘어가기 전에 별도의
리팩터링과 review를 수행한다. 이 gate는 성능 수치를 개선하는 작업이 아니라, 이번 구현이 불필요한 비용과
구조적 복잡성을 새로 남기지 않았는지 확인하는 절차다.

1. DDD 관점에서 message, buffer, native handle, socket, callback의 lifecycle과 ownership을 누가 소유하는지,
   state transition과 error mapping의 invariant가 어느 경계에 있는지 정리한다. Binding, raw FFI와 Core의
   책임을 섞거나 DDD 이름만 붙인 전달 계층을 만들지 않는다.
2. [`posddd.md`](../../../doc/principal/dev/posddd.md)의 POSD 기준으로
   shallow module, pass-through method, information leakage, temporal decomposition, 중복 abstraction과 호출자에게
   전가된 복잡성을 찾는다. 비자명한 변경은 두 가지 이상 대안을 비교한 뒤 책임 경계와 caller 부담이 더 작은
   안을 선택한다.
3. Public API에서 Core 호출까지의 production hot path를 다시 추적한다. 불필요한 allocation, payload·message
   copy, collection materialization, lock·GIL·mutex·channel·atomic contention과 작업마다 생성되는 thread,
   goroutine, task 또는 closure가 없어야 한다. 필요한 비용은 계약, C ABI 또는 memory safety 때문에 필요한
   이유와 owner를 cost inventory에 기록한다.
4. 제거한 Core service 계약에서 남은 export, wrapper, alias, branch, helper, test fixture, sample, perf scenario,
   build 설정, dependency, import와 주석을 찾는다. 실행되지 않거나 더 이상 계약을 지키지 않는 코드는 회귀
   test의 근거 없이 남기지 않고 삭제한다.
5. 리팩터링 뒤 영향받는 contract·unit test, optimization guard, sample process와 perf smoke를 다시 실행하고
   binding source manifest를 새로 만든다. 리팩터링 전 manifest나 test 결과는 최종 evidence로 재사용하지 않는다.

리팩터링이 끝나면 구현자가 아닌 frontier Codex coding/review agent가 고정된 source manifest와 전체 diff를 read-only로
검토한다. Contract와 architecture를 판단할 수 있는 model을 `high` 이상의 reasoning level로 사용하고, 선택한
model ID와 reasoning level을 언어별 review log에 기록한다. Finding은 `Critical`, `High`, `Medium`, `Low`로
분류하고 `contract`, `POSD`, `DDD`, `performance-cost`, `dead-code`, `test/evidence` category, `file:line`, 근거,
수정안과 재검증 방법을 기록한다.

`CLEAN`은 미해결 `Critical`, `High`, `Medium` finding이 0건이고, `Low` finding은 수정·기각 근거·후속 작업 중
하나로 처리했으며, 모든 필수 재검증이 같은 manifest에서 통과했다는 뜻이다. Review를 실행하지 않았거나
finding을 후속 성능 개선 작업으로 넘긴 상태는 `CLEAN`이 아니다. `NOT CLEAN`이면 finding을 수정하고 새
manifest와 fresh test 결과로 같은 review를 다시 수행한다. 언어별 문서는 이 gate가 `CLEAN`이 된 뒤에만 정식
문서, package와 clean consumer 단계로 진행한다. 실제 throughput·latency 개선은 최신화 완료 뒤 언어별 성능
개선 계획에서 별도로 진행한다.

### PGR-COMMON-01 — Candidate package 입력 전환

- Binding candidate tooling이 `V11-M3-CORE-PKG` evidence와 install prefix만 입력받게 한다.
- 언어별 source, test, package metadata와 build script의 content hash·mode를 기록할 manifest schema를 고정한다.
- Fixture source manifest를 격리 snapshot으로 materialize하고 변경된 content·mode·direct input을 거부하는
  regression을 추가한다.
- Package gate에서 repository `core/build`, `LD_LIBRARY_PATH`와 전역 header path를 거부한다.
- Core candidate identity나 platform runtime provenance가 달라지면 실패하는 regression을 추가한다.

PGR-COMMON-01은 schema, materializer와 fixture 검증까지만 완료한다. 실제 언어별 source 봉인과 격리
snapshot의 test·package·clean consumer 실행은 구현 후 각 언어 문서가 판정한다.

### PGR-COMMON-02 — Raw header·symbol inventory

- `core/tests/contract/check_public_surface.py`를 확장해 Core 0.9.0 정식 spec, install header와 runtime export의
  일치 결과를 machine-readable 목록과 hash로 출력한다.
- Header, runtime과 세 binding FFI를 비교할 machine-readable inventory 형식을 고정한다.
- Service header, allowlist 밖 symbol과 이전 SONAME을 fixture로 주입해 실패를 확인한다.

### PGR-COMMON-03 — Go·Rust parity 통합

- 공통 시작 gate에서는 Go·Rust 대응 메서드를 기록할 inventory의 열과 판정 규칙만 고정한다.
- 현재 Go·Rust submit 반환 차이와 boolean 정상 결과·함수군별 error 대안을 inventory에 기록한다. public
  signature를 바꾸는 경우에는 별도 공통 설계 review와 승인을 먼저 완료한다.
- 승인 결과와 현재 공통 오류 처리 정책, 비동기 완료 정책 및 Go·Rust 언어별 spec의 차이를 implementation
  gap으로 기록한다. Caller-provided receive의 no-data 규칙은 submit backpressure와 분리한다.
- Go와 Rust 작업은 성공 값, no-data, 함수군 error, `code`, `internal_errno`와 ownership을
  `bindings/doc/plan/go-rust-return-parity.ko.md`의 자기 언어 열에 각각 기록한다.
- Contract test는 public API로 실제 성공·실패 조건을 만들며 result code를 private hook으로 주입하지 않는다.
- 구현과 contract test가 통과한 뒤 공통 spec, 비동기 완료 정책, Go spec과 Rust spec의 한국어·영문 문서를
  승인된 초안에 맞추고 동기화 검사를 추가한다.
- 두 언어의 구현 후 품질 gate가 각각 `CLEAN`이 된 뒤 Codex parity review를 수행한다. 이 review가
  `CLEAN`이 되기 전에는 PGR-COMMON-03을 통과로 판정하지 않는다.

이 작업은 Go와 Rust 구현 중에 진행하며 언어별 전체 작업의 시작 조건은 아니다. 다만 submit 반환의 공통
설계 승인 substep은 GO-04와 RS-03의 signature 변경보다 먼저 끝내야 한다. 두 언어가 모두 완료된 뒤
PGR-COMMON-03 전체를 통과로 판정한다.

### PGR-COMMON-04 — 독립 실행 승인

PGR-COMMON-01과 PGR-COMMON-02가 끝나면 다음 값을 공통 log에 기록한다.

- Core candidate manifest absolute path와 SHA-256
- V11-R2 review evidence absolute path와 SHA-256
- V11-M3-CORE-PKG evidence absolute path와 SHA-256
- Core install prefix와 provenance SHA-256
- Raw header hash와 exported-symbol allowlist hash
- 현재 host에서 승인된 Core package build evidence의 absolute path와 SHA-256
- 언어별 목표 platform, artifact 생성 command의 존재 여부와 현재 준비 상태

PGR-COMMON-04는 언어별 platform artifact가 이미 생성됐음을 요구하지 않는다. 그 evidence는 시작 gate 이후
각 언어 실행 문서가 만든다. 승인된 host candidate의 hash와 경로가 확인되고, 목표 platform에서 artifact를
만들 수 없는 상태가 명시됐으면 PGR-COMMON-04를 통과로 표시한다. 이 기록이 있으면 Python, Go, Rust 담당자는
다른 언어의 진행 상태를 확인하지 않고 자기 실행 문서를 시작할 수 있다.

### PGR-COMMON-05 — 이전 bindings 설계 문서 정리

이전에 작성된 RouteMesh Python·Go·Rust 설계 문서는 Core service header를 계약 근거로 사용하므로 Core 0.9.0
raw-only 책임 경계의 구현 입력으로 사용할 수 없어 삭제했다. 아직 필요한 사용자 동작이 있으면 Framework
공통 정식 spec과 언어별 exact interface에서 계약 근거를 확인한다. 근거가 없는 내용은 Framework 설계 후보로
분리해 review한다. 삭제 이력은 [공통 정리 log](log/common/2026-08-04-draft-cleanup.ko.md)가 소유한다.

## 8. 통합 검증 표

| Gate | Common | Python | Go | Rust |
|------|--------|--------|----|------|
| 승인 Core candidate와 package provenance | `PENDING` | 입력 확인 | 입력 확인 | 입력 확인 |
| Raw header·symbol allowlist | `PENDING` | `PENDING` | `PENDING` | `PENDING` |
| 언어별 binding source manifest | 형식 고정 | `PENDING` | `PENDING` | `PENDING` |
| Source unit·contract test | 해당 없음 | `PENDING` | `PENDING` | `PENDING` |
| Hot path allocation·copy·contention review | 기준 고정 | `PENDING` | `PENDING` | `PENDING` |
| Perf runner smoke | 해당 없음 | `PENDING` | `PENDING` | `PENDING` |
| 구현 후 POSD·DDD 리팩터링과 Codex `CLEAN` review | 기준 고정 | `PENDING` | `PENDING` | `PENDING` |
| Go·Rust return-based parity | `PENDING` | 해당 없음 | `PENDING` | `PENDING` |
| Public contract draft 승인 | `PENDING` | naming | submit·context | submit·naming |
| Package contents와 provenance | 기반 준비 | `PENDING` | `PENDING` | `PENDING` |
| Source 밖 clean consumer | Core C | `PENDING` | `PENDING` | `PENDING` |
| Raw sample process runner | 해당 없음 | `PENDING` | `PENDING` | `PENDING` |
| 지원 platform native consumer | 기준 고정 | `PENDING` | `PENDING` | `PENDING` |
| 독립 review | `PENDING` | `PENDING` | `PENDING` | `PENDING` |

한 칸의 통과 결과로 다른 칸을 대신하지 않는다. 각 언어 문서는 자기 열의 상태와 evidence를 갱신하며,
공통 문서는 통합 판정만 반영한다.

## 9. 진행 기록

공통 실행 명령, 종료 코드, manifest hash와 실패 원인은 `bindings/doc/plan/log/common/` 아래 날짜별 log가
소유한다. 언어별 log 위치는 각 실행 문서가 정한다. 실패한 gate는 `BLOCKED`로 표시하고 재개할 정확한
입력과 명령을 기록한다.

## 10. 최종 완료 조건

다음 조건을 모두 만족해야 세 bindings의 Core 0.9.0 최신화가 완료된다.

1. 공통 candidate와 raw symbol allowlist gate가 통과한다.
2. Python, Go, Rust 실행 문서가 각각 완료 상태다.
3. Go·Rust parity inventory의 필수 행과 contract test가 통과한다.
4. Submit 반환과 언어별 naming의 parity 결정이 별도 승인된 뒤 구현·contract test와 정식 spec에 반영된다.
   승인되지 않은 설계 후보는 public contract로 취급하지 않는다.
5. 세 package가 같은 Core candidate identity와 platform별 runtime provenance를 기록하고, 각 언어의 test,
   package, clean consumer와 review가 같은 binding source manifest를 가리킨다.
6. 이전 service header, FFI, 공개 API, compatibility alias와 sample이 package에 없다.
7. Core service를 bindings 계약으로 정의한 이전 설계 문서가 삭제되고 필요한 Framework 요구의 소유 위치가
   확인된다.
8. 지원한다고 명시한 모든 platform에서 package 내부 runtime load가 검증된다.
9. 세 언어의 hot path review와 perf runner smoke가 통과한다. 성능 수치 개선은 이 완료 판정에 포함하지 않는다.
10. 각 언어의 구현 후 POSD·DDD 리팩터링, 성능 비용과 dead code 검토가 끝났고 Codex review가 `CLEAN`이다.
11. 미해결 `Critical`, `High`, `Medium` finding과 실행하지 않은 필수 gate가 남아 있지 않다.
