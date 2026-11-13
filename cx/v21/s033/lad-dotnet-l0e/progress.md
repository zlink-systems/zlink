# #4350 .NET L0 extension progress

## Receive/reply parity (checked before measurements)

| Work | Raw binding server | L0 Framework bench server | Match |
|---|---|---|---|
| wait/loop | `ZLinkRawServer/Program.cs:74-80`: 100 ms `ReceiveTimeout`, `router.Recv(received)` (blocking/default `None`), process exactly one record, then block again | `ZLinkServer/Program.cs:87-105`: same 100 ms `ReceiveTimeout`, `RecvFlags.None`, process exactly one record, then block again | yes |
| drain | none | none after removing the extra `DontWait` drain | yes |
| payload selection and parse | `ZLinkRawServer/Program.cs:76`: `PayloadPart`, `RawWire.Decode` | `ZLinkServer/Program.cs:101-102,174-175`: `PayloadPart`, one direct fixed `L0BenchHandler.HandleRequest/HandleSend`; `ParseFrom` | only allowed difference is fixed handler call |
| metrics | `BenchServerMetrics.RecordReceived/Record`; same per-message lock and counters | same methods through handler | yes; no extra per-message atomics |
| reply | `ZLinkRawServer/Program.cs:74-80,103-108`: new payload wrapper reuses body; allocate+encode one new body and one header; submit two parts | `ZLinkServer/Program.cs:101-107,174-185`: same body/header allocation and encoding, two-part submit | yes |
| copies | parsed protobuf and encoded new reply body; input part is viewed, not forwarded | same | yes |

No extra per-message wait, allocation, atomic, or copy remains in the L0 receive loop beyond the permitted fixed-handler call. The raw 100 ms receive timeout is also matched. The raw and L0 source excerpts above are the parity basis for every measurement below.

## Work

- Removed the L0 `DontWait` drain from `framework/bench/grpc/dotnet/ZLinkServer/Program.cs` because raw server processes one message per blocking receive.
- Existing compile-time L0 client transport switch: `framework/bench/grpc/dotnet/Client/Program.cs:167-170`; `ZLINK_DOTNET_LADDER_CLIENT=raw` selects `RawBenchTransport` with Framework-compatible envelope/RID. Will exercise it in the L0-server + L0-client cross cell.
- Existing results remain in `/root/jobs/fx/cx/v21/s033/lad-dotnet/raw`; this extension writes only under this directory.
- Measurement status: pending fresh build and locked measurements.

## Fresh build and measurement preflight — request-backpressure 1 KiB

- Build: `dotnet build framework/bench/grpc/dotnet/WithGrpcBench.sln -c Release --no-restore -m:1 -nodeReuse:false -p:UseSharedCompilation=false -p:ZLinkDotnetLadderLevel=L0`; success, 5 warnings, 0 errors (`raw/build-l0e.log`). Build was chained before binary freshness checks and measurement.
- Immediately before this measurement: `HEAD=40dc14b84f17b0aa44822eb2705c3e357c1940f0 dirty=yes`; `merge-base(HEAD, origin/main)=494c5f4827db8d86a5dce716b7a8da6e7e9cf970`.
- Worktree Release DLLs: Framework server `framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll`, SHA-256 `44a01a21e6fbcdb4fb7791a7115b200492c05d0a7ff313d7618a760375053ef9`; bench client `framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll`, `fe013217ebbe9281f8c792941a2dbe51b988e788022fac4940405cd6c718fc28`; raw binding server `framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll`, `7254db9fcc7f109ff4ff015e76c8b568db694bdad31ebaa9da350951a4b1d3c4`. Each DLL mtime is newer than its corresponding source file; all are under this worktree.
- receive/reply parity: `ZLinkRawServer/Program.cs:74-80,103-108` versus `ZLinkServer/Program.cs:87-107,174-185`. Both use one blocking receive per message with no drain; same metrics path/counters; one new protobuf body encode, one new response header and two-part submit. L0 adds only the permitted direct call to one fixed handler.
- Planned locked session: only `request-backpressure × 1024`, `zlink-framework-dotnet` (L0 raw client transport) and `zlink-dotnet` (binding), 5 runs each, 30 s warmup / 5 s active, source CPUs 0-7 / target CPUs 8-15; `tools/run-group.sh` holds `/root/jobs/fx/cx/measure.lock` exclusive across both implementations. Output `raw/request-backpressure-1k-5run/`.

- Discarded partial attempt `raw/request-backpressure-1k-5run/`: the wrapper used `PAYLOADS` instead of the runner's `PAYLOAD_SIZES`, so it also started 4 KiB and was interrupted before the binding run completed. The 1 KiB L0 run1 file is retained but is not part of the requested 5-run comparison. The corrected attempt uses a new output path and only `PAYLOAD_SIZES=1024`.

- Follow-up parity correction: raw sets a 100 ms receive timeout (`ZLinkRawServer/Program.cs:26-28`); L0 now sets the same timeout in `ZLinkServer/Program.cs:25`. An initial queued measurement was stopped before the lock opened, then rebuilt from the corrected source.

## Final fresh build and measurement preflight — request-backpressure 1 KiB

- Build: `dotnet build framework/bench/grpc/dotnet/WithGrpcBench.sln -c Release --no-restore -m:1 -nodeReuse:false -p:UseSharedCompilation=false -p:ZLinkDotnetLadderLevel=L0`; success, 5 warnings, 0 errors (`raw/build-l0e-measure3.log`). Build, freshness checks, and queued measurement are in one shell command.
- Immediately before this measurement: `HEAD=40dc14b84f17b0aa44822eb2705c3e357c1940f0 dirty=yes`; `merge-base(HEAD, origin/main)=494c5f4827db8d86a5dce716b7a8da6e7e9cf970`.
- Worktree Release DLLs: Framework server `framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll`, SHA-256 `63a3e07b6ac03b17ab722285fbfa821bb035d8a8c660d903a3fdf1430190882d`; bench client `framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll`, `fe013217ebbe9281f8c792941a2dbe51b988e788022fac4940405cd6c718fc28`; raw binding server `framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll`, `7254db9fcc7f109ff4ff015e76c8b568db694bdad31ebaa9da350951a4b1d3c4`. All paths are under this worktree and each DLL is newer than its corresponding source.
- Parity: raw receive `ZLinkRawServer/Program.cs:26-28,74-80`, L0 `ZLinkServer/Program.cs:25-26,88-107`; both use the same 100 ms blocking receive, one message per loop, no drain. Same metric calls/counters; same request parse, new response body encode/header allocation, two-part submit. L0 differs only by one call to the fixed handler.
- Measurement: only `request-backpressure × 1024`, L0 and binding in one run-local session, 5 runs each, 30 s warmup / 5 s active, CPUs 0-7 / 8-15, exclusive lock via copied `tools/run-group.sh`; output `raw/request-backpressure-1k-5run-final/`.

## Cross-test preflight — send-saturation 1 KiB, three pairings

- Immediately before enqueueing this measurement: `HEAD=40dc14b84f17b0aa44822eb2705c3e357c1940f0 dirty=yes`; `merge-base(HEAD, origin/main)=494c5f4827db8d86a5dce716b7a8da6e7e9cf970`.
- Build/provenance is the fresh Release build recorded above: Framework server DLL `framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll` SHA-256 `63a3e07b6ac03b17ab722285fbfa821bb035d8a8c660d903a3fdf1430190882d`; client DLL `framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll` `fe013217ebbe9281f8c792941a2dbe51b988e788022fac4940405cd6c718fc28`; raw binding server DLL `framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll` `7254db9fcc7f109ff4ff015e76c8b568db694bdad31ebaa9da350951a4b1d3c4`. All under this worktree; all DLLs are newer than their sources.
- receive/reply parity: raw `ZLinkRawServer/Program.cs:26-28,74-80,103-108`; L0 `ZLinkServer/Program.cs:25-26,88-107,174-185`. Same 100 ms blocking receive, one-message loop/no drain, metrics counters, request parse, one newly allocated+encoded reply body, one header, two-part submit; only L0's direct fixed-handler call differs.
- Cross session: three pairings, each `send-saturation × 1024`, 3 runs each, 30 s warmup / 5 s active, source CPUs 0-7 / target CPUs 8-15, one exclusive lock via copied `tools/run-group.sh`; Framework L0 client pairing sets `ZLINK_DOTNET_LADDER_CLIENT=raw`. Outputs: `raw/cross-l0/raw-raw/`, `raw/cross-l0/framework-raw/`, `raw/cross-l0/framework-l0-client/`.

## request-backpressure 1 KiB, five-run comparison

- All 10 runs passed: `errors=0`, `server_errors=0`, `abandoned=0`, `submitted=completed=target_stats.received`. G5 passed for both implementations (L0 spread 2.4%, binding spread 5.2%).
- Throughput medians: L0 `98,489.2 msg/s`; binding `102,599.0 msg/s`; L0/binding `0.960`, so this cell now passes the `0.95` threshold. Aggregated with the 5-run median: `raw/aggregates/request-backpressure-1k-5run.md` / `.json`; raw runs `raw/request-backpressure-1k-5run-final/run{1..5}/zlink-framework-dotnet-request-backpressure-1024/results.json` and corresponding `zlink-dotnet-...` files.
- Individual rates (L0 / binding): run1 `98,489.2 / 97,371.8`; run2 `99,383.4 / 102,599.0`; run3 `97,135.8 / 102,915.2`; run4 `96,170.2 / 100,630.4`; run5 `100,122.0 / 107,960.6` msg/s.
- Compared with the former 3-run result `0.946`, the 5-run median clears the target. Per-message allocation evidence from the original report remains approximately equal (`1456 B/receive` for both); this rerun measured throughput only.
- Cross runner environment correction before it acquired the lock: the raw/raw pairing leaves `ZLINK_DOTNET_RAW_TARGET` unset; the Framework-server/raw-client pairing sets it to `framework`; the Framework-server/L0-client pairing unsets it and selects the compile-time L0 raw client with `ZLINK_DOTNET_LADDER_CLIENT=raw`. No cross measurement had started when this correction was made.

## L0 cross test — send-saturation 1 KiB

| Server | Client | Run 1 | Run 2 | Run 3 | Median | vs raw/raw | G5 | Raw |
|---|---|---:|---:|---:|---:|---:|---|---|
| raw binding | raw binding | 442,836.0 | 402,633.0 | 442,412.6 | 442,412.6 | 1.000 | 9.0%, pass | `raw/cross-l0/raw-raw/run{1,2,3}/zlink-dotnet-send-saturation-1024/results.json` |
| Framework L0 | raw binding | 443,342.6 | 442,139.6 | 429,042.8 | 442,139.6 | 0.999 | 3.0%, pass | `raw/cross-l0/framework-raw/run{1,2,3}/cross-zlink-dotnet-to-zlink-framework-dotnet-send-saturation-1024/results.json` |
| Framework L0 | Framework bench client in L0 raw mode | 416,613.8 | 429,604.8 | 440,273.8 | 429,604.8 | 0.971 | 3.0%, pass | `raw/cross-l0/framework-l0-client/run{1,2,3}/zlink-framework-dotnet-send-saturation-1024/results.json` |

- All 9 cross runs passed: `errors=0`, `server_errors=0`, `abandoned=0`, final `submitted=completed=target_stats.received`. Warmup 30 s / active 5 s, source CPUs 0-7 / target CPUs 8-15. Each pairing has its own normalized aggregate under `raw/aggregates/cross-*.md` / `.json`.
- The failed normal Framework-client route-mesh pairing from the previous report is replaced by the L0 bench client transport. L0 mode routes through the binding-shaped raw socket transport selected at `framework/bench/grpc/dotnet/Client/Program.cs:167-170` by `ZLINK_DOTNET_LADDER_CLIENT=raw`; both the L0 server and this client mode passed the cross send cell.
