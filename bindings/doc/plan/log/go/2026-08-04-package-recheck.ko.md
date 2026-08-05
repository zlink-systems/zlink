# Go binding Core 0.9.0 package 재검증 기록

이 기록은 2026-08-04에 `427fbce0f5c` 시점의 Go binding source로 생성하고 candidate identity를 연결한 fresh6 package
evidence를 정리한다. Linux
x86_64 package·clean consumer·sample·perf smoke 범위는 통과했지만, Go 작업 전체는 공통 submit 계약 승인,
독립 frontier review와 다른 platform native consumer가 남아 `PARTIAL / NOT CLEAN`이다.

## Candidate와 package evidence

| 항목 | 값 |
|------|-----|
| Candidate manifest | `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json` |
| Candidate manifest SHA-256 | `d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765` |
| Candidate aggregate SHA-256 | `327587596195a162374498b630f51a043977dd392eb556061af615bf05186703` |
| Core package provenance SHA-256 | `46f7bd17c0be3987fed14ca3cb594139e3edb778d3996248d23b6d2d6b53f693` |
| Core runtime SHA-256 | `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4` |
| Binding source revision | `427fbce0f5c0a3b6000506380b3d40521ed86413` |
| Binding source manifest SHA-256 | `3240b10c68ad6dfb1ebe08a8ec27a6ea526a3b02ff48f59ed5c20b0573a59cff` |
| Binding source aggregate SHA-256 | `982a24119f27f032b7cac9cf7e8a691631797be7b89a931a3d6c87334114fc91` |
| Module zip SHA-256 | `76f1d83f76c6203765f67938392c199f6d6441fc714f18c1c1e7f7611e57b274` |
| Package evidence SHA-256 | `4ae453178ceb1a7bcaebe8994a39479eac14a86a9f8146642c503df7006888a2` |
| Package platforms | `linux-x86_64` |

Package evidence는 `zlink.systems/zlink@v0.9.0` module, package-local Core 0.9.0 header와 candidate runtime을
사용한다. `cleanConsumer`는 `replace` 없이 실제 message roundtrip과 module-cache runtime load를 통과했다.
Package에는 service·Spot·Actor·build·results forbidden entry가 없다.
Source manifest는 package 생성 시점의 HEAD `427fbce0f5c`를 기록하며, 이후 계획 문서 commit에서도 `bindings/go`에는
변경이 없다.

```bash
git diff --name-status 6d698c7e68e0c263ee48dd3948e7b8cc6e865c7d..427fbce0f5c0a3b6000506380b3d40521ed86413 -- bindings/go
```

위 명령은 출력을 만들지 않았다. 따라서 fresh6은 binding source가 바뀌지 않은 상태에서 source provenance를
`427fbce0f5c`에 고정한 package evidence다.

## 실행 결과

- extracted fresh6 package: `go test ./...`, `go test -race ./...`, `go vet ./...`, raw/hot-path guard와 samples
  `pass=7 fail=0`
- fresh6 package `go test ./...`, `go test -race ./...`, `go vet ./...`와 guard: exit code `0`
- Single smoke: `PAIR`, `inproc`, message size `64`, duration `1`, run `1`, exit code `0`
- Multi smoke: `MULTI_DEALER_ROUTER`, `tcp`, clients `1`, message size `64`, duration `1`, run `1`, exit code `0`

Perf smoke는 extracted package의 `native/linux-x86_64/libzlink.so.0.1.0`을 출력하고 runtime SHA-256
`b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4`를 확인했다. Single smoke의 결과 행은
throughput `1259306.000`, latency `0.034`, p95 `0.120`, p99 `0.195`이고 multi smoke는 throughput
`9045.000`, latency `0.055`, p95 `0.068`, p99 `0.110`이다. 이 값은 실행 의미를 확인하기 위한 출력이며
공식 report나 성능 수치 개선을 증명하지 않는다. package에 없는 Python report helper를 호출하지 않는
`--smoke` 경로로 실행했다.

## 설계 검토와 남은 조건

Go raw cgo boundary, ownership·error mapping, request progress owner와 hot-path cost inventory는 현재
계획의 PASS 범위에 있다. callback handle lifecycle race도 `go test -race ./...`에서 재검증했으며, Service
projection과 Core 10 compatibility surface도 제거했다.

다음 조건은 이 package evidence만으로 닫히지 않는다.

1. Go·Rust submit 반환 초안은 아직 공통 승인되지 않았다. 현재 Go terminal method의 `(bool, error)`와
   request completion 정책을 승인 전에는 `error` only로 바꾸지 않는다.
2. 현재 V11-R2 review는 `coordinator_self_review`, `independent: false`이다. 다른 candidate에 대한 과거
   independent review evidence를 현재 candidate `d318...`에 재사용하지 않는다.
3. Linux arm64 payload는 Core 9 `libzlink.so.9`이고, Darwin payload는 이 candidate runtime과 native
   consumer로 검증되지 않았다. Windows는 현재 Go 계획 범위 밖이다.
4. 따라서 parity inventory의 행이 채워졌더라도 최종 parity 판정, 정식 common contract 문서와 GO-07의 독립
   `CLEAN` review를 완료로 기록하지 않는다.
