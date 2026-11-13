# Java call-shape: L0, L1, L2

**NOT a restored feature — L0/L1/L2 target receive: after the first blocking receive, the ladder drains with `RecvFlags.DONT_WAIT`; raw target uses `RecvFlags.NONE` on every receive.** (`zlink-framework-server/ZLinkFrameworkBenchServer.java:134-143`; `zlink-raw-server/ZLinkRawBenchServer.java:94-105,155-160`.)

**NOT a restored feature — L1 target receive: same `DONT_WAIT` drain difference.** (`ZLinkFrameworkBenchServer.java:134-143` vs `ZLinkRawBenchServer.java:94-105,155-160`.)

**NOT a restored feature — L2 target receive: same `DONT_WAIT` drain difference.** (`ZLinkFrameworkBenchServer.java:134-143` vs `ZLinkRawBenchServer.java:94-105,155-160`.)

비교 범위: `zlink-java`의 `RawStack` raw binding 경로와 `zlink-framework-java`의 ladder operation. L0/L1/L2는 동일한 ladder server entry를 사용하며 환경 변수로 L1 permit과 L2 envelope decode를 추가한다. 요청-응답 행은 request scenario, one-way 행은 command scenario의 해당 분기를 가리킨다.

## L0

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — legacy envelope + protobuf body 두 부분, request/send submit | `java/client/.../RawStack.java:66-88,167-190` | `java/client/.../FrameworkStack.java:131-144` → `RawStack.java:66-88,167-190` | yes | — |
| Target wait/receive — blocking `NONE` 대 first `NONE` 후 `DONT_WAIT` drain | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:94-105,155-160` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:123-180,194-225` | no | **NOT a restored feature** |
| Target per-message metrics — body receipt와 active/latency counter update | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:109,136-137`; `java/shared/.../BenchServerMetrics.java:44-57` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:213-218`; same `BenchServerMetrics.java:44-57` | yes | — |
| Target payload decode — protobuf body를 decode | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:104-109,135-137` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:194-218` | yes | — |
| Target reply build/submit — 새 legacy response envelope + protobuf body를 2-part submit | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:110-127` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:219-224` | yes | — |
| Source completion/reply receive — binding completion poller와 reply callback/decode | `java/client/.../RawStack.java:79-88,237-264,274-290` | ladder operation이 raw stack의 submit/reply callback 사용 `java/client/.../FrameworkStack.java:131-144` → `RawStack.java:79-88,237-264,274-290` | yes | — |
| Source per-message metrics — submitted/completed/in-flight atomic counters와 latency samples | `java/client/.../BenchDrivers.java:694-719` | 같은 `BenchDrivers.SourceMetrics` `BenchDrivers.java:694-719` | yes | — |

## L1

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — legacy envelope + protobuf body 두 부분, request/send submit | `java/client/.../RawStack.java:66-88,167-190` | `java/client/.../FrameworkStack.java:131-144` → `RawStack.java:66-88,167-190` | yes | — |
| Target ingress permit — binding raw server에는 application job queue permit 없음 | 없음 (`java/zlink-raw-server/.../ZLinkRawBenchServer.java:94-105`) | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:123-152,174-183` | no | L1 permit |
| Target wait/receive — blocking `NONE` 대 first `NONE` 후 `DONT_WAIT` drain | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:94-105,155-160` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:134-180` | no | **NOT a restored feature** |
| Target per-message metrics — body receipt와 active/latency counter update | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:109,136-137`; `java/shared/.../BenchServerMetrics.java:44-57` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:213-218`; same `BenchServerMetrics.java:44-57` | yes | — |
| Target payload decode — protobuf body를 decode | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:104-109,135-137` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:194-218` | yes | — |
| Target reply build/submit — 새 legacy response envelope + protobuf body를 2-part submit | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:110-127` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:219-224` | yes | — |
| Source completion/reply receive — binding completion poller와 reply callback/decode | `java/client/.../RawStack.java:79-88,237-264,274-290` | ladder operation이 raw stack의 submit/reply callback 사용 `java/client/.../FrameworkStack.java:131-144` → `RawStack.java:79-88,237-264,274-290` | yes | — |
| Source per-message metrics — submitted/completed/in-flight atomic counters와 latency samples | `java/client/.../BenchDrivers.java:694-719` | 같은 `BenchDrivers.SourceMetrics` `BenchDrivers.java:694-719` | yes | — |

## L2

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — Framework envelope header encode 후 protobuf body와 두 부분 request/send submit | `java/client/.../RawStack.java:66-88,167-190` | `java/client/.../FrameworkStack.java:136-159` → `RawStack.java:93-125,133-135,209-230` | no | L2 envelope header |
| Target ingress permit — binding raw server에는 application job queue permit 없음 | 없음 (`java/zlink-raw-server/.../ZLinkRawBenchServer.java:94-105`) | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:123-152,174-183` | no | L1 permit |
| Target wait/receive — blocking `NONE` 대 first `NONE` 후 `DONT_WAIT` drain | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:94-105,155-160` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:134-180` | no | **NOT a restored feature** |
| Target envelope header decode/validation | 없음 | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:200-212` | no | L2 envelope header |
| Target per-message metrics — body receipt와 active/latency counter update | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:109,136-137`; `java/shared/.../BenchServerMetrics.java:44-57` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:213-218`; same `BenchServerMetrics.java:44-57` | yes | — |
| Target payload decode — protobuf body를 decode | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:104-109,135-137` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:213-218` | yes | — |
| Target reply build/submit — 새 legacy response envelope + protobuf body를 2-part submit | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:110-127` | L0/L1은 `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:232-234`; L2는 아래 추가 행 참조 | L0/L1 yes; L2 adds production response header | L2 response envelope |
| Source completion/reply receive — binding completion poller와 reply callback/decode | `java/client/.../RawStack.java:79-88,237-264,274-290` | L0/L1 body validate `RawStack.java:274-290`; L2는 아래 추가 행 참조 | L0/L1 yes; L2 adds production response header parse | L2 response envelope |
| Source per-message metrics — submitted/completed/in-flight atomic counters와 latency samples | `java/client/.../BenchDrivers.java:694-719` | 같은 `BenchDrivers.SourceMetrics` `BenchDrivers.java:694-719` | yes | — |
| L2 target reply — request header로 response header를 만들고 protobuf body와 두 부분 submit | `java/zlink-raw-server/.../ZLinkRawBenchServer.java:110-127` | `java/zlink-framework-server/.../ZLinkFrameworkBenchServer.java:216-226,237-269` → production `ZLinkChannelDispatchReporter.java:378-397`의 `reply(requestHeader)` + prepared-name `encode` | no | L2 response envelope |
| L2 source completion — 응답 header 한 번 parse 후 두 번째 part에서 protobuf body decode | `java/client/.../RawStack.java:79-88,274-290` | `java/client/.../RawStack.java:126-132,404-433` → production `ZLinkChannelCallRuntime.java:260-272`의 `tryDecodeHeader` 1회 후 reply decode | no | L2 response envelope |

경로 표기에서 `java/client/...`는 `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/`이고, `java/zlink-raw-server/...`, `java/zlink-framework-server/...`, `java/shared/...`도 각각 같은 `framework/bench/grpc/java/` 아래의 명시 디렉터리다.
