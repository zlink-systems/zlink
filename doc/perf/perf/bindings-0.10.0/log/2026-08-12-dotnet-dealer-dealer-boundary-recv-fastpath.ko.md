# .NET multi DEALER/DEALER C parity와 recv hot path

`MULTI_DEALER_DEALER` server는 active deadline 뒤에 queued message를 계속 count하고,
large message teardown을 C보다 길게 유지했다. server poller에 deadline timer를 함께 등록해
active window를 종료하고, ready socket은 C와 같이 첫 DONT_WAIT recv 뒤 deadline을 확인한
다음 drain한다. timer는 poller보다 먼저 만들고 poller가 먼저 dispose되도록 수명 순서를
정했다.

Sol review 결과 DEALER recv hot path의 두 후보를 적용했다. `zlink_dealer_recv_part`를
source-generated `LibraryImport` Cdecl call로 바꾸고, native `zlink_msg_init` 직전의
불필요한 `ZlinkMsg` zero-initialization을 `Unsafe.SkipInit`으로 바꿨다. 두 변경은
public interface와 Core C API를 변경하지 않는다. send operation wrapper pool은 public
operation의 보관·재호출 계약을 깨뜨릴 수 있어 적용하지 않았다.

## 검증

release Core `0.10.1`과 .NET Release build에서 다음을 통과했다.

```text
dotnet build perf/multi/Zlink.BindingBench.Multi/Zlink.BindingBench.Multi.csproj --no-restore -c Release
```

C 다음 .NET을 한 번씩 실행했다. 조건은 tcp, client 100, duration 1초, auto-HWM,
message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-dotnet-dd-recvfast-c/multi/report/perf_c_multi_linux_20260812_225920.txt` |
| .NET | `/tmp/zlink-dotnet-dd-recvfast-dotnet/multi/report/perf_dotnet_multi_linux_20260812_225935.txt` |

| Size | C throughput | .NET throughput | .NET/C |
|---:|---:|---:|---:|
| 64B | 2627597 | 882665 | 33.59% |
| 256B | 1456766 | 853129 | 58.56% |
| 1024B | 537799 | 798039 | 148.39% |
| 4096B | 286610 | 225199 | 78.57% |
| 65536B | 75047 | 103366 | 137.74% |
| 131072B | 48093 | 44136 | 91.77% |
| 산술평균 | - | - | 91.44% |

산술평균은 .NET 단순 one-way 목표 85%를 통과했다.
