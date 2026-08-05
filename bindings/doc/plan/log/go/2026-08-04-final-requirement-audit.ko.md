# Go binding Core 0.9.0 완료 조건 최종 대조

이 기록은 `go-core-11-update.ko.md`의 완료 조건 11개를 현재 source, package evidence와 review 입력에
대조한 결과다. 문서의 누락을 찾기 위한 audit이며, `PARTIAL / NOT CLEAN`인 gate를 완료로 바꾸지 않는다.

## 기준 입력

| 항목 | 값 |
|------|-----|
| Go source revision | `427fbce0f5c0a3b6000506380b3d40521ed86413` |
| Candidate manifest SHA-256 | `d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765` |
| Candidate aggregate SHA-256 | `327587596195a162374498b630f51a043977dd392eb556061af615bf05186703` |
| Go source manifest SHA-256 | `3240b10c68ad6dfb1ebe08a8ec27a6ea526a3b02ff48f59ed5c20b0573a59cff` |
| Go package evidence | `.artifacts/wsl/go-candidate-final6/go-package-v0.9.0.json` |
| Go package evidence SHA-256 | `4ae453178ceb1a7bcaebe8994a39479eac14a86a9f8146642c503df7006888a2` |
| Module zip SHA-256 | `76f1d83f76c6203765f67938392c199f6d6441fc714f18c1c1e7f7611e57b274` |
| Core runtime SHA-256 | `b6fadc481c649b50637a9c0eb01d15a016e6ba4cd5bab967bdb6da4497a3c0c4` |
| Current V11-R2 review | `passed`, `independent: false`, `coordinator_self_review` |

## 조건별 판정

| # | 완료 조건 | 판정 | 현재 증거와 남은 조건 |
|---|-----------|------|----------------------|
| 1 | `/v11@v0.9.0`, 승인 candidate와 source manifest가 같은 package evidence에 기록됨 | `PASS` | fresh6 package evidence와 clean consumer가 module, current source revision, candidate identity를 연결한다 |
| 2 | Raw cgo·public API가 Core 0.9.0 allowlist에 맞고 service API가 없음 | `PASS` | raw allowlist, public surface test와 package forbidden-entry 검사가 통과했다 |
| 3 | Error·no-data·ownership이 Go–Rust parity 열과 일치함 | `PARTIAL` | 현재 Go evidence는 있으나 submit 반환과 callback completion 의미의 공통 승인이 없다 |
| 4 | Submit·Context 규칙이 승인 draft와 contract test에 일치함 | `PARTIAL` | Context cancellation/deadline test는 통과했지만 submit draft 승인과 error-only 결정이 없다 |
| 5 | Source test, vet, hot-path guard, perf smoke와 raw sample이 통과함 | `PASS` | `427fbce0f5c` source로 생성한 fresh6 package에서 `go test`, race, `go vet`, guard, single·multi smoke와 samples `7/7`을 통과했다 |
| 6 | POSD·DDD·비용·dead code 검토와 독립 `CLEAN` review가 끝남 | `PARTIAL` | 자체 review와 `go test -race`는 통과했지만 현재 candidate의 독립 frontier review가 없다 |
| 7 | replace 없는 clean consumer가 package runtime으로 message 송수신함 | `PASS` | fresh6 builder의 `cleanConsumer: pass`와 module-cache ldd/roundtrip 증거가 있다 |
| 8 | 지원 Linux·macOS platform의 동일 candidate runtime load가 검증됨 | `PARTIAL` | Linux x86_64만 통과했다. Linux arm64는 `libzlink.so.9`, macOS 두 target은 native consumer 미검증이며 현재 Go package builder도 `linux-x86_64` 외 target을 거부한다 |
| 9 | Go spec, GoDoc와 guide가 구현·공통 contract와 일치함 | `PARTIAL` | Go 문서는 현재 구현과 맞고 submit draft도 branch에 있지만, 초안 승인과 parity 반영은 아직 전이다 |
| 10 | 성능 수치 개선을 완료 근거로 사용하지 않음 | `PASS` | smoke는 실행 의미만 확인하고 공식 성능 개선 판정은 하지 않았다 |
| 11 | 미해결 Critical/High/Medium finding과 미실행 필수 gate가 없음 | `PARTIAL` | 독립 review, submit approval과 non-x86_64 consumer gate가 남아 있다 |

## Platform package builder 재확인

`scripts/local-package/go/build-wsl.sh`의 `platform_source_dir`는 현재 `linux-x86_64`만 source payload로
허용한다. 같은 candidate manifest와 Core package evidence를 넣어 다른 target을 요청한 결과는 다음과 같다.

| 요청 target | 종료 코드 | 결과 |
|------------|----------|------|
| `linux-aarch64` | `2` | `Go package platform is not present in the supplied Core candidate` |
| `darwin-x86_64` | `2` | `Go package platform is not present in the supplied Core candidate` |
| `darwin-aarch64` | `2` | `Go package platform is not present in the supplied Core candidate` |

따라서 현재 non-x86_64 gate는 Linux host에서 실행할 수 없다는 범위만의 문제가 아니다. 동일 Core candidate
runtime을 해당 target의 package에 넣을 builder 입력과 native consumer 실행 환경이 모두 필요하다. 기존
major 9 payload나 다른 workstream의 11.2.0 artifact를 이 candidate의 증거로 대체하지 않는다.

현재 Core worktree의 11.2.0 candidate와 기존 Go 승인 candidate를 비교한 결과도 별도 log에 기록했다.
기존 V11-R2 review는 새 candidate SHA `483df3ff20925fda60b3ac5a1c75e71e47c5eab871242623ee2c7fa66dd644bd`를
거부했고, 기존 candidate를 현재 worktree에 검증하면 `core/CMakeLists.txt` content drift로 종료 코드 `1`이다.
이 결과는 Go 11.1.0 package evidence를 유지해야 하는 근거이며, 새 Core candidate의 승인으로 해석하지 않는다.
자세한 명령과 출력은
[`log/go/2026-08-04-current-candidate-recheck.ko.md`](2026-08-04-current-candidate-recheck.ko.md)에 기록했다.

## 최종 상태

Go plan의 문서·source·Linux x86_64 package 범위는 `427fbce0f5c` source의 fresh6 evidence로 재현 가능하다. 이후
변경은 `bindings/go`에 없음을 확인했다. 그러나 조건 3, 4, 6, 8,
9, 11이 남아 있으므로 최종 상태는 `PARTIAL / NOT CLEAN`이다.

다음 closure에는 우회 구현이 아니라 아래 외부 입력이 필요하다.

1. PGR-COMMON-03에서 Go·Rust submit 반환과 Context/callback completion 정책을 승인하고 contract test를
   추가한다.
2. 그 승인 결과와 동일한 Go source manifest에 대해 구현자와 분리된 frontier reviewer가 전체 diff를 읽고
   `CLEAN` manifest를 만든다.
3. 동일 Core candidate runtime을 포함한 Linux arm64와 macOS package를 만들고 각 host에서 clean consumer와
   runtime load를 실행한다.
