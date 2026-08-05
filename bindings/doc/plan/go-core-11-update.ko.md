# Go binding Core 0.9.0 최신화 실행 계획

> 대상 독자는 Go binding의 module, cgo bridge와 platform payload를 갱신하는 담당자와 reviewer다. 이 문서는
> “승인된 Core candidate를 받아 Go 작업만 독립적으로 완료하려면 무엇을 바꾸고 어떤 package consumer를
> 통과해야 하는가?”에 답한다.

## 1. 시작 조건과 현재 상태

[공통 계획](python-go-rust-core-11-update.ko.md)의 공통 시작 gate인 PGR-COMMON-01, PGR-COMMON-02와
PGR-COMMON-04가 통과하면 Python과 Rust의 진행 상태와 관계없이 이 작업을 시작할 수 있다. 시작 log에는
Core candidate identity, V11-R2 review, V11-M3-CORE-PKG evidence, install prefix와 raw symbol allowlist hash를
기록한다.

현재 Go module은 `zlink.systems/zlink`이고 package version은 `v0.9.0`이다. `bindings/go/include/zlink.h`와
Linux x86_64 runtime은 Core 0.9.0.1.0 raw contract를 사용한다. Linux aarch64에 추적된 payload는 major 9이며,
Darwin payload는 현재 Core 0.9.0 runtime으로 검증되지 않았다. Root의 `VERSION`과 `core/include/zlink.h`에 있는
11.2.0 변경은 다른 workstream의 dirty change이므로 이 작업의 package 입력으로 사용하지 않는다.

현재 Go source, 승인 Core 0.9.0.1.0 runtime, raw sample과 candidate identity를 연결한 local file-proxy consumer의 범위는
통과했다. Service API, Spot, Actor와 MeshNode projection은 제거했다. 따라서 현재 판정은 **PARTIAL / NOT
CLEAN**이다. 전체 완료를 막는 조건은 다음과 같다.

사용자의 명시 요청에 따라 구현자 자체 검토와 승인을 다시 수행했으며, 결과는
[`log/go/2026-08-04-self-review-approval.ko.md`](log/go/2026-08-04-self-review-approval.ko.md)에 기록했다.
이 승인은 Go source, POSD·DDD refactoring, Linux x86_64 package와 clean consumer 범위에만 적용하며,
독립 review로 가장하지 않는다.

- Go package는 현재 Core candidate의 `V11-M3-CORE-PKG` pass evidence와 연결되지만, V11-R2 review가
  `independent: false`이므로 독립 review gate는 닫히지 않았다.
- Linux arm64와 macOS payload의 Core 0.9.0 runtime 및 native consumer가 검증되지 않았다.
- 현재 `scripts/local-package/go/build-wsl.sh`도 `linux-x86_64`만 candidate payload source로 허용한다. `linux-aarch64`,
  `darwin-x86_64`, `darwin-aarch64` 요청은 모두 exit 2로 거부되므로 non-x86_64 gate에는 target payload와
  builder 확장, native consumer 실행 환경이 함께 필요하다.
- 현재 Core worktree의 11.2.0 candidate는 기존 Go V11-R2 review가 승인하지 않으며, 기존 승인 candidate는
  현재 `core/CMakeLists.txt` drift 때문에 worktree에 재사용할 수 없다. 두 검증 결과는
  [`log/go/2026-08-04-current-candidate-recheck.ko.md`](log/go/2026-08-04-current-candidate-recheck.ko.md)에 기록했다.
- Go–Rust parity inventory의 대응 행은 채워졌지만 공통 submit 반환 초안과 parity 판정은 아직 승인 전이다. 현재
  Go send/request terminal method는 `(bool, error)`를 유지하므로 “성공 값 없는 submit은 `error`만 반환한다”는
  목표를 완료로 표시하지 않는다.
- 구현자가 아닌 frontier reviewer의 POSD·DDD·성능 비용·dead code 판정이 없어 `CLEAN`으로 닫을 수 없다.

Candidate verify 입력은 다음과 같다.

| 항목 | 값 |
|------|-----|
| Candidate evidence | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json` |
| Candidate file SHA-256 | `d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765` |
| Candidate aggregate SHA-256 | `327587596195a162374498b630f51a043977dd392eb556061af615bf05186703` |
| Candidate base revision | `73a9ce6d5bf275e9675333fc01e50948dbf895a2` |
| V11-R2 review | `.artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json` |
| V11-R2 review SHA-256 | `171a9cc8f7203500de08050dcb74ecd36b4c9ce55a75a14ce1bee283705c9e04` |
| Review 판정 | `passed`, `independent: false` |
| V11-M3-CORE-PKG evidence | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json` |
| V11-M3-CORE-PKG evidence SHA-256 | `3a912391c6a7d441e22e606c3407e626ee9d05883142cc541b331498f39722df` |
| Approved runtime SHA-256 | `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4` |

현재 Core worktree와 기존 승인 candidate의 재사용 검증은
[`log/go/2026-08-04-current-candidate-recheck.ko.md`](log/go/2026-08-04-current-candidate-recheck.ko.md)에 기록했다.

## 2. Module path와 version 결정

Service API 제거는 breaking change이고 binding package는 Core 0.9.0 major/minor에서 다시 시작한다. 이 실행
계획의 module은 다음과 같다.

```text
module zlink.systems/zlink
version v0.9.0
```

Go semantic import versioning에 따라 source, test, sample, perf와 consumer import는 `/v11`로 함께 사용한다.
Module source는 commit된 `bindings/go` snapshot에서 materialize한다. Package script는 binding source의
미커밋 변경을 거부하고, 선택한 platform runtime은 package evidence에 별도로 기록한다.

## 3. 목표와 범위

Go module은 승인된 Core 0.9.0 raw C API만 투영한다. Context, message, raw socket, monitor, poller, timer와
utility를 Go 관례에 맞게 제공하고 service API와 이전 Core runtime을 포함하지 않는다.

다음 작업은 범위 밖이다.

- Framework service runtime 구현
- Windows cgo linker와 loader 신규 지원
- 새 Core API 설계
- 외부 module proxy 게시와 release tag 생성
- 이번 작업의 완료 근거로 사용할 성능 수치 개선

## 4. 구현 작업과 현재 판정

### GO-01 — Module과 공개 entrypoint — PASS

- `go.mod`는 `zlink.systems/zlink`을 선언한다.
- Root package와 `contracts` package는 Core 0.9.0 raw 계약을 projection한다.
- Source, sample, perf와 GoDoc은 `/v11` module path와 공개 entrypoint를 사용한다.
- Internal package와 cgo type은 public signature와 GoDoc에 노출되지 않는다.

### GO-02 — Raw cgo inventory — PASS

- `internal/native/ffi.go`와 package-local header는 Core 0.9.0 raw allowlist로 검사한다.
- cgo include path는 package 안의 `include/`만 사용하며 repository `core/include`를 package consumer에 노출하지
  않는다.
- Package header tree에서 `zlink/service/`와 이전 service include를 제거했다.
- Header에 없는 service function, struct, enum과 callback 선언을 제거했다.
- `bindings/go/tests/raw-core11-allowlist.json`과 `TestRawCore11Allowlist`가 header set, header SHA-256, public
  C symbol과 local helper를 machine-readable 형태로 검사한다. Allowlist SHA-256은
  `740a1ff9289a65be0c53777325fad25ea0fe326c91959e04711fc0a71af095e2`이다.

### GO-03 — Service API 제거 — PASS

- `contracts/service_spot.go`를 제거했다.
- `internal/native`와 root projection에서 MeshNode, Spot, Actor, service snapshot, bridge와 관련 operation을
  제거했다.
- Service callback, service header, Core 10 alias와 compatibility export를 유지하지 않는다.
- Raw public surface gate와 package zip forbidden-entry 검사가 service path와 symbol의 잔존을 막는다.

### GO-04 — Error와 ownership — PARTIAL

완료한 범위는 함수군별 error가 `Code()`와 `InternalErrno()`를 제공하도록 정리하고 `NativeErrno` public
surface를 제거한 것이다. Context는 terminal native submit 전에 cancellation/deadline을 검사하며,
`errors.Is`로 `context.Canceled`와 `context.DeadlineExceeded`를 확인하는 test가 있다. Message send, receive,
copy·move와 close ownership test도 raw runtime 기준으로 실행한다.

현재 공개 terminal signature는 다음과 같다.

```go
SendSubmitOp.Submit(context.Context) (bool, error)
RequestSubmitOp.SubmitAsync(context.Context) (<-chan RequestReplyCompletion, error)
RequestSubmitOp.Submit(context.Context, RequestReplyCallback) (bool, error)
ReplySubmitOp.Submit(context.Context) error
```

따라서 `Send`와 request callback submit의 `false, nil` backpressure semantics는 현재 계약이다. 이전에
작성된 error-only 반환 설계 후보는 공통 승인을 받지 못해 삭제했으며, 그 후보를 근거로 signature를 바꾸지
않는다. Go–Rust parity inventory에서 차이를 계속 기록하고, public signature를 바꾸려면 별도 공통 승인과
정식 contract 갱신이 필요하다.

### GO-05 — Hot path 설계 검토 — PASS (독립 review 제외)

Hot path 비용은 `bindings/go/tests/hot-path-cost-inventory.json`에 owner, 이유와 guard test를 연결했으며
`unclassified`는 0건이다. 주요 판단은 다음과 같다.

- Message native allocation, snapshot copy, submit 보존 copy와 received adoption은 ownership contract에
  필요한 비용으로 분류했다.
- `getPubBytesOption`에서 Go-owned scratch buffer를 다시 복사하던 경로를 제거했다.
- Request마다 progress polling worker와 timer를 만들지 않고 `socketCore`가 보관하는 handle 단위 progress
  pump를 사용한다. Request마다 completion을 기다리는 lightweight waiter goroutine은 active handle 수명과
  결과 전달을 위해 남아 있으며 inventory에 required 비용으로 기록했다.
- Callback worker, native option scratch buffer와 package runtime rpath는 현재 실행 의미를 바꾸지 않는 필수
  비용으로 분류했다.
- 독립 socket의 hot path에 package 전역 lock을 추가하지 않았고, raw callback·buffer ownership은 내부 owner가
  관리한다.

다음 gate가 통과했다.

```bash
go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory)' -count=1
```

### GO-06 — Sample과 perf smoke — PASS (Linux x86_64)

- Pair, pub/sub, dealer/router request, STREAM receive·packet callback과 monitor의 raw sample process가
  `pass=7 fail=0`으로 통과했다.
- Spot, Actor와 service operation sample·perf scenario는 runner에서 제거했다.
- Sample과 perf는 public root package만 사용하며 private cgo bridge와 raw byte 우회를 사용하지 않는다.
- Single runner는 승인 package `native` runtime path를 출력한다. 현재 Linux x86_64 runtime SHA-256은
  `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4`이며 다음 smoke가 통과했다.

```bash
perf/run_benchmarks.sh --smoke --pattern PAIR --duration 1 --msg-sizes 64 --transports inproc --runs 1
```

- Multi runner도 같은 package runtime 기준으로 다음 TCP smoke가 통과했다.

```bash
perf/run_benchmarks_multi.sh --smoke --pattern MULTI_DEALER_ROUTER --duration 1 --msg-sizes 64 --transports tcp --runs 1 --clients 1
```

Smoke는 ready, active, 필수 `RESULT` metric과 exit code만 확인하며 공식 성능 report를 만들지 않는다.
Package zip에는 Python report helper가 포함되지 않으므로 `--smoke` 경로는 해당 helper를 호출하지 않는다.
이 경계는 `6d698c7e68`에서 고정했고, `427fbce0f5c` source로 생성한 fresh6 extracted package smoke에서 다시 확인했다.

### GO-07 — 구현 후 POSD·DDD 리팩터링과 Codex review — PASS (self-review scope)

현재까지 다음 설계 결정을 적용했다.

- DDD 경계를 Core raw adapter, Context/resource lifecycle, Message buffer ownership, operation builder,
  receive adoption, context cancellation과 `socketCore` request progress owner로 나누었다. Go cancellation은 Core
  function-group error mapping과 별도 경계로 유지한다.
- POSD 관점에서 service alias와 forwarding projection을 제거하고, 호출자가 native pointer·callback userdata·
  codec·transport detail을 조립하지 않도록 내부에서 감쌌다.
- `getPubBytesOption`의 불필요한 복사를 제거하고, request progress polling의 lifetime owner를
  `socketCore`로 올렸다. native handle key 전역 map 대신 socket lifecycle에 pump 참조를 두고, 외부
  `Poller`와 공유해야 하는 handle registry만 전역으로 유지했다. 이 선택은 caller surface를 늘리지
  않으면서 ownership과 비용을 한 owner에 모으는 방향이다.
- callback handle registration, replacement와 close 해제를 `socketCore` mutex 아래에서 직렬화하고,
  dispatcher close는 mutex 밖에서 수행했다. `go test -race ./...`에서 확인된 `OnPacket`–`Close` data
  race를 이 owner 경계에서 제거했다.
- 구현 단위별 검증과 path-limited commit/push를 다음 checkpoint로 남겼다.

| Checkpoint | Commit | 내용 |
|------------|--------|------|
| Raw contract boundary | `afd96c43aa` | Core 0.9.0 raw projection, service surface 제거, `/v11`, HWM contract |
| Error/lifecycle/perf boundary | `eab6cf9411` | Error surface, context test, bounded progress pump, sample/perf smoke |
| Gate boundary | `f1210adaffc` | Raw allowlist, hot-path inventory, package builder와 clean-consumer gate |
| Candidate provenance boundary | `40dcadb2ef0` | 승인 Core candidate와 package evidence를 검증하는 Go package builder |
| Perf runtime boundary | `c6b37ac0ee` | Root `VERSION` 대신 Go package header에서 native SONAME version을 해석 |
| Lifecycle owner boundary | `e9be2c8c46` | `socketCore`가 request progress와 callback handle lifecycle을 소유하고 callback close race를 제거 |
| Package smoke boundary | `6d698c7e68` | Go package perf `--smoke`가 package 외부 Python report helper 없이 동작하도록 고정 |
| Package source boundary | `427fbce0f5c` | 이 source revision으로 candidate identity를 연결한 package와 clean consumer, race·sample·perf smoke를 재검증 |
| Platform builder boundary | `3740c59ad9` | non-x86_64 package builder 거부 결과와 같은 candidate를 요구하는 platform gate를 기록 |
| Submit design boundary | `7f27ed6bc5` | 미승인 Go·Rust submit 반환 설계 후보를 기록했으나 채택하지 않았고 public signature는 바꾸지 않음 |
| Runtime close boundary | `935b0407fc` | multipart ownership, completion errno, Poller completion state와 request progress close ordering을 수정하고 race·package consumer를 재검증 |

사용자의 명시 요청에 따라 위 범위를 구현자 자체 검토로 다시 읽고 승인했다. 재검증 결과는
[`log/go/2026-08-04-self-review-approval.ko.md`](log/go/2026-08-04-self-review-approval.ko.md)에 있다.
다만 구현자가 아닌 frontier reviewer의 read-only 전체 diff review evidence는 없다. V11-R2 Core review도
`independent: false`이므로 이 결과를 Go 독립 review로 대체하지 않는다. 따라서 자체 검토 범위는 승인했지만,
독립 `CLEAN` review를 요구하는 최종 완료 gate는 별도로 열려 있다.

이번 자체 검토의 위험 신호, 대안 비교, 변경 경계와 source gate 결과는
[`log/go/2026-08-04-posd-ddd-self-review.ko.md`](log/go/2026-08-04-posd-ddd-self-review.ko.md)에 기록했다.

### GO-08 — 정식 문서 — PARTIAL

Go 정식 spec의 한국어·영문 문서는 `/v11` module, Core 0.9.0 raw public API, ownership, receive no-data, error
surface와 package boundary에 맞춰 갱신했다. `bindings/go/README.godoc.md`, `tests/run_tests.sh`와 sample
entrypoint도 현재 구현과 맞는다.

공통 bindings spec의 submit 반환 설계 후보는 공통 승인을 받지 못해 삭제했다. 현재 Go spec과 구현의 반환
signature는 유지되며, Go–Rust parity inventory의 최종 판정과 공통 문구 통합은 아직 남아 있다. 따라서 언어별
Go spec 갱신만으로 전체 public contract parity 완료를 주장하지 않는다.

## 5. Platform 검증

현재 cgo linker 설정은 `internal/native/ffi.go`, payload는 `native/`에 있다. 다음 표는 이 source에 분기가 있는
target만 나타낸다.

| Platform | 상태 | 근거와 남은 조건 |
|----------|------|------------------|
| Linux amd64 (`linux-x86_64`) | `PASS` | V11-M3-CORE-PKG 승인 runtime SHA-256 `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4`, package clean consumer와 ldd path 검증 |
| Linux arm64 (`linux-aarch64`) | `BLOCKED` | `libzlink.so.9`, SHA-256 `9c3cf64ca56e15b9f2200e33d377d5ce8f19d4fcc180a941f6a5c51442170391`; Core 0.9.0 candidate runtime과 consumer가 없음 |
| macOS amd64 (`darwin-x86_64`) | `UNVERIFIED` | payload SHA-256 `16b52674acbdac834c98727b8cd58228a65d993c738b35176dea939e0940b2a1`가 현재 Core 0.9.0 candidate와 연결되지 않았고 이 Linux host에서 native consumer를 실행하지 않음 |
| macOS arm64 (`darwin-aarch64`) | `UNVERIFIED` | payload SHA-256 `6e6dc25c0360b7cb3a1d79f67ec5161924458653948b7a2cc78ddffaf86a3d8d`가 현재 Core 0.9.0 candidate와 연결되지 않았고 이 Linux host에서 native consumer를 실행하지 않음 |
| Windows | 범위 밖 | 별도 cgo linker·loader 구현 전에는 release 범위 밖 |

Linux와 macOS 각 artifact는 같은 Core candidate identity를 사용해야 하며 runtime hash와 loader evidence를
platform별로 기록한다. 현재 Linux x86_64 PASS만 local package gate에 포함한다. `scripts/local-package/go/build-wsl.sh`의
현재 구현은 `linux-x86_64` 외 platform을 `Go package platform is not present in the supplied Core candidate`로
거부한다. 이 확인 결과와 각 target의 종료 코드는
[`log/go/2026-08-04-final-requirement-audit.ko.md`](log/go/2026-08-04-final-requirement-audit.ko.md)에 기록했다.

## 6. Go module package와 clean consumer

Repository subtree를 tar로 묶거나 consumer `go.mod`에 local path `replace`를 넣는 방식은 package 완료 증거로
사용하지 않는다. `scripts/local-package/go/build-wsl.sh`는 명시한 V11-M3-CORE-VERIFY candidate manifest와
V11-M3-CORE-PKG evidence를 먼저 검증한 뒤, commit된 `bindings/go` source snapshot에서 module을 materialize한다.
Binding source의 미커밋 변경, package-local header version drift와 candidate와 다른 native payload를 거부한다.

```text
<proxy>/zlink.systems/zlink/@v/v0.9.0.info
<proxy>/zlink.systems/zlink/@v/v0.9.0.mod
<proxy>/zlink.systems/zlink/@v/v0.9.0.zip
```

현재 package command는 다음 입력을 사용한다.

```bash
scripts/local-package/go/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest /absolute/path/candidate-reply-match-completion-hwm-20260801.json \
  --core-package-evidence /absolute/path/core-package-20260801.json \
  --output-root /absolute/path/.artifacts/wsl/go-candidate
```

현재 Linux x86_64 package evidence는 다음 값을 기록한다.

| 항목 | 값 |
|------|-----|
| Source revision | `935b0407fcac58332bf6b4f02e468d7b93564adc` |
| Source manifest SHA-256 | `7af1abe3d43a7a55592d60fa79ea01861fe1c650d823610b4f78d01d5fedfb1c` |
| Package script SHA-256 | `9404def1079805c0cf200244f670deaae55daba17031095594aa099dc09b3fee` |
| Module zip SHA-256 | `e12cdaf3b83d15b3135daf9b8f741bca50c24269dec7cd2fcc21398fed3ed67d` |
| Header aggregate SHA-256 | `159c8024f8ed090e0c3acfe51e665339d3a43e93b37dc9e21490b703df717f1d` |
| Source aggregate SHA-256 | `937fb0f27b88aff801c33dbdfe251a6650973cd98693abdbc502555ec9a43aed` |
| Package evidence | `.artifacts/wsl/go-candidate-final7/go-package-v0.9.0.json` |
| Package evidence SHA-256 | `e0ac97b387322843a28b0f328ebf51ab395d81e6b4af588954b8559f1b1b6991` |
| Core candidate manifest | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json` |
| Core package evidence | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json` |
| Core provenance SHA-256 | `46f7bd17c0be3987fed14ca3cb594139e3edb778d3996248d23b6d2d6b53f693` |
| Runtime SHA-256 | `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4` |
| Clean consumer | `pass`, `replace` 없음, module-cache runtime ldd 확인 |

이 evidence는 Linux x86_64 package가 동일한 Core candidate manifest, V11-R2 review와 installed Core provenance를
사용했음을 증명한다. 독립 Go review와 다른 platform package evidence를 대신하지 않는다.

## 7. 검증 표

| Gate | 상태 | Evidence |
|------|------|----------|
| 공통 candidate 입력 확인 | `PASS` (독립 review 제외) | Candidate verify와 matching V11-M3-CORE-PKG pass evidence가 같은 manifest `d318...`/aggregate `327...`을 사용하며 runtime provenance 검증 통과 |
| Go binding source manifest | `PASS` | `.artifacts/wsl/go-candidate-final6/go-source-manifest-v0.9.0.json`, SHA-256 `3240b10c68ad6dfb1ebe08a8ec27a6ea526a3b02ff48f59ed5c20b0573a59cff` |
| `/v11` module path와 version | `PASS` | `bindings/go/go.mod`, package evidence `v0.9.0` |
| Raw cgo·header·symbol allowlist | `PASS` | `bindings/go/tests/raw-core11-allowlist.json`, `TestRawCore11Allowlist` |
| Public API snapshot과 service 부재 | `PASS` | root/contracts projection, raw surface test, package zip forbidden-entry 검사 |
| `go test ./...` | `PASS` | fresh6 extracted package와 `bindings/go/tests/run_tests.sh`에서 통과 |
| `go vet ./...` | `PASS` | fresh6 extracted package와 `bindings/go/tests/run_tests.sh`에서 통과 |
| `go test -race ./...` | `PASS` | callback handle lifecycle와 request progress owner 변경 후 Linux x86_64에서 통과 |
| Hot path cost inventory와 optimization guard | `PASS` | `hot-path-cost-inventory.json`, `TestHotPathCostInventory`, `TestOptimizationGuard` |
| Perf runner smoke | `PASS` | `427fbce0f5c` source의 fresh6 extracted package에서 single PAIR inproc와 multi DEALER/ROUTER TCP smoke, exit 0 |
| 구현 후 POSD·DDD·성능 비용·dead code Codex review | `NOT CLEAN` | 현재 독립 frontier review와 fresh finding report 없음 |
| Go·Rust parity inventory | `PARTIAL` | 대응 surface·error·ownership 행은 기록했지만 현재 submit 반환 차이, contract test와 parity `CLEAN` 판정이 남음 |
| Submit·`context.Context` contract | `PARTIAL` | cancellation/error tests와 현재 `false, nil` semantics는 확인했지만 통합 반환 shape 결정은 미완료 |
| Raw sample process runner | `PASS` | `samples: pass=7 fail=0` |
| File proxy package contents | `PASS` | candidate identity를 기록한 zip에 Linux runtime 포함, service/build/results forbidden-entry 없음 |
| Replace 없는 clean module consumer | `PASS` | 빈 `GOMODCACHE`/`GOCACHE`, go build와 Pair roundtrip, module-cache ldd |
| Linux·macOS native consumer | `PARTIAL` | Linux x86_64 PASS. Linux arm64와 macOS 요청은 현재 builder가 exit 2로 거부하고, 같은 candidate runtime을 사용한 native consumer evidence도 없음 |
| 한국어·영문 spec, GoDoc와 guide | `PARTIAL` | Go spec/GoDoc/sample entrypoint 갱신; 공통 parity 통합 미완료 |
| Package·통합 최종 review | `PENDING` | Linux x86_64 candidate 연결은 통과했지만 다른 platform evidence와 독립 review 필요 |

명령, 종료 코드, test 수, module zip SHA-256과 실패 원인은
`bindings/doc/plan/log/go/` 아래 날짜별 log에 기록한다.
11개 완료 조건의 현재 증거 대조는
[`log/go/2026-08-04-final-requirement-audit.ko.md`](log/go/2026-08-04-final-requirement-audit.ko.md)에 기록했다.

## 8. 완료 조건

다음 조건을 모두 만족해야 Go 작업을 완료한다.

1. Module은 `zlink.systems/zlink@v0.9.0`이며 승인 Core candidate identity와 Go binding source manifest가
   같은 package evidence에 기록된다.
2. Raw cgo와 공개 API가 Core 0.9.0 allowlist에 맞고 service API가 없다.
3. 함수군별 error, no-data와 ownership이 parity inventory의 Go 열과 일치한다.
4. Submit 반환 규칙과 `context.Context` cancellation·deadline semantics가 공통 draft 승인 결과와 정식
   contract test에 일치한다. 현재 `(bool, error)`를 임의로 error-only로 바꾸지 않는다.
5. Source test, `go vet`, hot path guard, perf smoke와 raw sample process가 통과한다.
6. POSD·DDD 리팩터링, 불필요한 allocation·copy·contention과 dead code 검토가 끝났고 독립 Codex review가
   `CLEAN`이다.
7. Replace 없는 clean consumer가 file proxy package의 runtime으로 실제 message를 송수신한다.
8. 지원 대상 Linux·macOS platform에서 같은 candidate identity의 package contents와 runtime load가 검증된다.
9. 정식 Go spec, GoDoc와 guide가 구현과 common contract에 일치한다.
10. 성능 수치 개선은 후속 Go 계획으로 분리되어 있으며 이번 완료 근거로 사용하지 않는다.
11. 미해결 `Critical`, `High`, `Medium` finding과 실행하지 않은 필수 gate가 남아 있지 않다.
