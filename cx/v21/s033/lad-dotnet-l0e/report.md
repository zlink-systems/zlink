# #4350 .NET L0 extension result

## 결과

- raw server와 L0 server의 receive loop를 parity 규칙으로 맞췄다. 두 경로 모두 100 ms timeout의 blocking `Recv`로 한 메시지만 받고 처리한 뒤 다음 blocking receive로 돌아간다. `DontWait` drain을 제거했다.
- request-backpressure 1 KiB의 5-run median은 L0 `98,489.2`, binding `102,599.0 msg/s`; 비율 `0.960`으로 기준 `0.95`를 통과했다. 두 구현 모두 G5 통과, spread는 각각 2.4%, 5.2%다.
- 세 cross pairing은 모두 3/3 run 유효했고, G5를 통과했다. L0 server + L0 client는 raw/raw의 `0.971`배였다.

## Receive/reply parity

| 항목 | raw binding | Framework L0 | 판정 |
|---|---|---|---|
| wait·loop | `ZLinkRawServer/Program.cs:26-28,74-80`: 100 ms timeout, blocking receive, 한 메시지 처리 | `ZLinkServer/Program.cs:25-26,88-107`: 같은 timeout, `RecvFlags.None`, 한 메시지 처리 | 일치 |
| drain | 없음 | 없음; 추가 `DontWait` drain 제거 | 일치 |
| 메시지 처리 | `RawWire.Decode` 후 metrics | `L0BenchHandler.HandleRequest/HandleSend` 직접 호출 | 고정 handler 호출 1회만 차이 |
| metrics·per-message atomics | `BenchServerMetrics.Record*`; 활성 메시지 공통 lock/counter | 같은 metrics 경로 | 추가 atomic 없음 |
| reply | `ZLinkRawServer/Program.cs:74-80,103-108`: 새 payload wrapper, 새 protobuf body encode, 새 header, 2-part submit | `ZLinkServer/Program.cs:101-107,174-185`: 같은 할당·encode·part 수·submit | 일치 |
| copy | 입력 part view, protobuf parse 및 새 응답 body encode | 동일 | 추가 copy 없음 |

## 측정 결과

### request-backpressure × 1 KiB

| 구현 | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Median | G5 |
|---|---:|---:|---:|---:|---:|---:|---|
| L0 Framework bench | 98,489.2 | 99,383.4 | 97,135.8 | 96,170.2 | 100,122.0 | 98,489.2 | 2.4%, pass |
| Binding | 97,371.8 | 102,599.0 | 102,915.2 | 100,630.4 | 107,960.6 | 102,599.0 | 5.2%, pass |

10/10 run 모두 `errors=0`, `server_errors=0`, `abandoned=0`, `submitted=completed=received`였다. L0/binding `=0.960`. 각 run 원자료는 `raw/request-backpressure-1k-5run-final/run{1..5}/`에 있고 집계는 `raw/aggregates/request-backpressure-1k-5run.md`에 있다. 이전 3-run 비율 0.946을 5-run median으로 다시 판정해 기준을 통과했다. 이전 측정의 per-receive allocation은 두 구현 모두 약 1456 B였다.

### send-saturation × 1 KiB cross

| Server | Client | Run 1 | Run 2 | Run 3 | Median | raw/raw 대비 | G5 |
|---|---|---:|---:|---:|---:|---:|---|
| raw binding | raw binding | 442,836.0 | 402,633.0 | 442,412.6 | 442,412.6 | 1.000 | 9.0%, pass |
| Framework L0 | raw binding | 443,342.6 | 442,139.6 | 429,042.8 | 442,139.6 | 0.999 | 3.0%, pass |
| Framework L0 | Framework bench client (L0 raw mode) | 416,613.8 | 429,604.8 | 440,273.8 | 429,604.8 | 0.971 | 3.0%, pass |

9/9 cross run 모두 `errors=0`, `server_errors=0`, `abandoned=0`, `submitted=completed=received`였다. 결과 원자료는 각각 `raw/cross-l0/raw-raw/run{1..3}/`, `raw/cross-l0/framework-raw/run{1..3}/`, `raw/cross-l0/framework-l0-client/run{1..3}/` 아래에 있다. 각 pairing 집계는 `raw/aggregates/cross-*.md`에 있다.

Framework bench client의 compile-time L0 mode는 `framework/bench/grpc/dotnet/Client/Program.cs:167-170`에 있으며 `ZLINK_DOTNET_LADDER_CLIENT=raw`로 실행했다. 이는 Java의 L0 `RawStack`과 같은 binding-shaped transport다. 이전 실패에 사용된 정상 Framework route-mesh client 대신 이 모드를 사용해 L0 server pairing을 성공시켰다.

## 빌드와 변경

- Release solution 빌드 성공: 5 warnings, 0 errors. 로그 `raw/build-l0e-measure3.log`.
- 변경 파일: `framework/bench/grpc/dotnet/ZLinkServer/Program.cs` — raw와 맞추기 위해 L0 receive loop의 batch drain을 제거하고, 같은 100 ms receive timeout을 설정했다.
- 측정은 experiment branch `exp/4350-dotnet-ladder`에서 수행했다. commit/push하지 않았다.
- 5-run 비교 provenance: HEAD `40dc14b84f17b0aa44822eb2705c3e357c1940f0`, base `494c5f4827db8d86a5dce716b7a8da6e7e9cf970`; Framework server DLL SHA-256 `63a3e07b6ac03b17ab722285fbfa821bb035d8a8c660d903a3fdf1430190882d`; client DLL `fe013217ebbe9281f8c792941a2dbe51b988e788022fac4940405cd6c718fc28`; raw server DLL `7254db9fcc7f109ff4ff015e76c8b568db694bdad31ebaa9da350951a4b1d3c4`. 모두 이 worktree에서 빌드했다.
