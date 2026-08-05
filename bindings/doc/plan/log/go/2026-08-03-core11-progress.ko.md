# Go binding Core 0.9.0 진행 log

작성일: 2026-08-03

이 log는 Go binding 작업에서 확인한 사실과 현재 미완료 조건을 분리해 기록한다. Root `VERSION`,
`core/include/zlink.h`, 다른 binding과 framework의 dirty change는 이 작업의 변경 범위에 포함하지 않았다.

## 현재 판정

Go source와 Core 0.9.0 raw projection은 Linux x86_64에서 test, sample process, perf smoke와 candidate-bound
file-proxy clean consumer를 통과했다. 현재 전체 작업은 `PARTIAL / NOT CLEAN`이다. Go package는
`V11-M3-CORE-PKG` pass evidence와 같은 candidate identity를 사용하지만 V11-R2 review가
`independent: false`이다. Go–Rust parity, common submit draft 승인, Linux arm64/macOS payload와 독립
frontier review가 남아 있다.

## Candidate 입력

```text
candidate:
  path: .artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json
  sha256: d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765
  aggregateSha256: 327587596195a162374498b630f51a043977dd392eb556061af615bf05186703
  baseRevision: 73a9ce6d5bf275e9675333fc01e50948dbf895a2
review:
  path: .artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json
  sha256: 171a9cc8f7203500de08050dcb74ecd36b4c9ce55a75a14ce1bee283705c9e04
  status: passed
  independent: false
```

`V11-M3-CORE-PKG` evidence는 다음 candidate identity와 일치한다.

```text
packageEvidence:
  path: .artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json
  sha256: 3a912391c6a7d441e22e606c3407e626ee9d05883142cc541b331498f39722df
candidateManifestSha256: d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765
candidateAggregateSha256: 327587596195a162374498b630f51a043977dd392eb556061af615bf05186703
runtimeSha256: b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4
provenanceManifestSha256: 46f7bd17c0be3987fed14ca3cb594139e3edb778d3996248d23b6d2d6b53f693
status: pass
```

## 구현과 설계 checkpoint

다음 path-limited checkpoint를 각각 commit하고 origin branch에 push했다.

| Commit | 범위 |
|--------|------|
| `afd96c43aa` | Core 0.9.0 raw projection, service API·header·sample·perf 제거, `/v11`, HWM `uint64` 전달 |
| `eab6cf9411` | `ZlinkError`/`InternalErrno`, context cancellation test, bounded handle progress pump, raw sample/perf runner |
| `f1210adaffc` | raw header/symbol allowlist, hot-path cost inventory, Go file-proxy package와 clean-consumer gate |
| `40dcadb2ef0` | 승인 Core candidate manifest/package evidence를 검증하는 package builder와 candidate-bound Linux payload |
| `c6b37ac0ee` | perf runner가 root `VERSION` 대신 Go package header에서 native SONAME version을 해석 |

POSD와 DDD 판단은 다음 책임 경계를 기준으로 적용했다.

- Core raw adapter는 native pointer, callback userdata, codec와 transport detail을 내부에 둔다.
- Message, Received, operation builder와 request progress handle은 각각 buffer ownership, receive adoption,
  submit state와 completion lifetime의 owner다.
- Context cancellation은 Core function-group error mapping과 분리한다.
- Go-owned scratch buffer를 반환 전에 다시 복사하던 경로를 제거했으며, ownership에 필요한 native allocation,
  snapshot copy, submit preservation copy와 callback worker는 cost inventory에 분류했다.
- Service alias, forwarding projection, Core 10 compatibility export와 dead service path를 남기지 않았다.
- Package builder가 candidate manifest, Core package evidence, installed provenance와 runtime/header hash를
  한 경계에서 검증하게 해 호출자가 여러 hash를 수동으로 조합하지 않도록 했다. 이 검증은 package 입력의
  provenance 책임을 builder에 둔 POSD 정보 은닉과 DDD의 Core package 경계를 반영한다.
- Perf runner는 다른 workstream의 root `VERSION`을 읽지 않고 Go package header에서 SONAME version을 읽는다.
  따라서 package runtime 선택이 binding package 경계 안에 머물고, unrelated root version drift가 실행 결과를
  바꾸지 않는다.

## 검증 명령과 결과

실행 위치는 별도로 표시하지 않은 경우 `bindings/go`이다.

```bash
bash bindings/go/tests/run_tests.sh
```

결과: 종료 코드 `0`. `go test ./...`, `go vet ./...`, raw allowlist·optimization·hot-path guard와 process
sample이 통과했다. Sample 결과는 `pass=7 fail=0`이다.

```bash
go test -count=2 ./...
go vet ./...
go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory)' -count=1
```

결과: 세 명령 모두 종료 코드 `0`.

```bash
perf/run_benchmarks.sh --smoke --pattern PAIR --duration 1 --msg-sizes 64 --transports inproc --runs 1
perf/run_benchmarks_multi.sh --smoke --pattern MULTI_DEALER_ROUTER --duration 1 --msg-sizes 64 --transports tcp --runs 1 --clients 1
```

결과: 두 명령 모두 종료 코드 `0`. 두 runner는 `libzlink.so.0.1.0`과 runtime SHA-256
`b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4`를 출력했다. Single runner는 `PAIR`
inproc, multi runner는 TCP `MULTI_DEALER_ROUTER`에서 READY/active와 필수 `RESULT` metric을 출력했다.
Smoke 실행은 공식 report를 만들지 않았다.

최종 source revision `c6b37ac0ee2278305bbcb848a17bfc1034af86d3`에서 별도 임시 `GOCACHE`와 `GOTMPDIR`을
사용해 `go test -count=1 ./...`와 `go vet ./...`를 다시 실행했다. 두 명령 모두 종료 코드 `0`이다.

```bash
scripts/local-package/go/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest \
    /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json \
  --core-package-evidence \
    /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json \
  --output-root /home/hep7/project/kairos/zlink/.artifacts/wsl/go-candidate-final2
```

결과: module `zlink.systems/zlink`, version `v0.9.0`, candidate-bound clean consumer `pass`.
Consumer는 빈 `GOMODCACHE`와 `GOCACHE`에서 `replace` 없이 module을 다운로드·build하고 Pair message
roundtrip을 실행했다. `ldd`는 module cache의 `native/linux-x86_64/libzlink.so.0`을 가리켰다.

현재 package evidence:

```text
evidence: .artifacts/wsl/go-candidate-final2/go-package-v0.9.0.json
evidence sha256: 6c6c46ecaa24e6c3c4ce622310ff030fb0db5996090872b1d75d7d0505e15724
sourceRevision: c6b37ac0ee2278305bbcb848a17bfc1034af86d3
sourceManifestSha256: cce6140e65c2853814816888e6f1edaae00a52a36edd41b1d6b988b9402bd75b
packageScriptSha256: 9404def1079805c0cf200244f670deaae55daba17031095594aa099dc09b3fee
moduleZipSha256: 9be68867732ddea0fc3a2c44cd32bb0d7785b8d2df7f02fb3beaa7ad458fb264
headerSha256: 159c8024f8ed090e0c3acfe51e665339d3a43e93b37dc9e21490b703df717f1d
sourceSha256: b7bc9db75879874b3bc128d69192e70eb5c04922c1ec47c047108299f2edd002
runtimeSha256: b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4
coreCandidateManifestSha256: d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765
coreCandidateAggregateSha256: 327587596195a162374498b630f51a043977dd392eb556061af615bf05186703
corePackageEvidenceSha256: 3a912391c6a7d441e22e606c3407e626ee9d05883142cc541b331498f39722df
coreProvenanceManifestSha256: 46f7bd17c0be3987fed14ca3cb594139e3edb778d3996248d23b6d2d6b53f693
```

```bash
unzip -Z1 /home/hep7/project/kairos/zlink/.artifacts/wsl/go-candidate-final2/proxy/zlink.systems/zlink/@v/v0.9.0.zip \
  | rg 'service|spot|actor|/build/|/results/'
```

결과: 금지된 zip entry가 출력되지 않았다. Package script는 기존 zip을 임시 파일로 만든 뒤 교체하므로
이전 실행의 삭제된 service entry가 산출물에 남지 않는다.

현재 다른 platform payload는 다음 이유로 package 입력에서 제외했다.

```text
linux-aarch64/libzlink.so.9:
  sha256: 9c3cf64ca56e15b9f2200e33d377d5ce8f19d4fcc180a941f6a5c51442170391
  result: Core 9 SONAME; candidate-bound Core 0.9.0 package evidence 없음
darwin-x86_64/libzlink.dylib:
  sha256: 16b52674acbdac834c98727b8cd58228a65d993c738b35176dea939e0940b2a1
  result: candidate provenance와 Linux host loader evidence 없음
darwin-aarch64/libzlink.dylib:
  sha256: 6e6dc25c0360b7cb3a1d79f67ec5161924458653948b7a2cc78ddffaf86a3d8d
  result: candidate provenance와 Linux host loader evidence 없음
```

## 계약 문서

- `bindings/doc/spec/go/README.ko.md`와 `README.en.md`는 `/v11` raw Core 0.9.0 public contract, current
  `(bool, error)` submit signature, ownership, no-data와 error semantics를 반영한다.
- `bindings/go/README.godoc.md`와 sample runner는 구현된 public root projection을 기준으로 한다.
- Common submit draft 승인과 Go–Rust parity inventory 통합은 별도 gate로 남긴다.

## 남은 작업

1. Linux arm64의 major 9 payload와 Darwin payload를 같은 Core 0.9.0 candidate runtime으로 교체한 뒤 native
   consumer와 loader evidence를 실행한다.
2. Go–Rust parity inventory와 submit 반환 draft를 승인하고, 현재 signature를 유지할지 error-only로 바꿀지
   공통 contract에 반영한다.
3. 구현자가 아닌 frontier reviewer가 같은 source manifest와 fresh test 결과를 read-only로 검토한다. `Critical`,
   `High`, `Medium` finding이 없고 최종 `CLEAN` 판정이 있어야 전체 완료로 올린다.
