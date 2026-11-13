| 구간 | Framework p50/p90 (µs) | Binding p50/p90 (µs) | 차이 Framework−Binding p50/p90 (µs) |
|---|---:|---:|---:|
| A 출발 송신→target 수신 | 11.2 / 15.6 | 17.8 / 27.8 | −6.6 / −12.2 |
| B target 수신→target 송신 | 106.7 / 147.0 | 50.7 / 77.7 | +56.0 / +69.4 |
| C target 송신→출발 수신 | 20.9 / 28.7 | 18.9 / 27.7 | +2.0 / +1.1 |
| D 출발 수신→다음 출발 송신 | 126.7 / 172.9 | 70.0 / 109.7 | +56.7 / +63.2 |

## Framework 쪽에서 더 긴 두 구간

1. B의 target 수신 뒤 .NET 수신 worker handoff, route dispatch, handler와 reply 제출까지의 syscall 경계 구간이다. Framework는 p50/p90 106.7/147.0µs이고 Binding은 50.7/77.7µs로, 관측 차이는 +56.0/+69.4µs다. Framework 경로는 `ZLinkManagedMeshNode.ReceiveLoopAsync`와 `DrainRawSocket`, `ZLinkProcessExecutionPool.ReceiveUntilHandoff`, `ZLinkMeshNodeRouteDispatcher.DispatchNodeRouteAsync`, `ZLinkRouteHandlerInvoker.InvokeRequestAsync`를 지난다. Binding은 `RunRequestRouter`와 `ReplyMultipart`에서 수신·처리·reply를 잇는다.

2. D의 출발 수신 뒤 completion과 caller continuation을 거쳐 다음 송신까지의 syscall 경계 구간이다. Framework는 p50/p90 126.7/172.9µs이고 Binding은 70.0/109.7µs로, 관측 차이는 +56.7/+63.2µs다. Framework CPU 표본에서 `CompletionOwner.Drain`, `RequestCompletionEntry.MoveReply`, `ZLinkCompletionDispatcher.WorkItem.Execute`, `ExecuteRequestAsync`와 `ObserveRequestReplyAsync`가 확인된다. Binding 경로는 `CompletionOwner.Drain`, `RequestCompletionEntry`와 `RequestAsync`다.

위 두 수치는 B와 D 전체의 경계 간 시간이다. 기록에서 각 관리 코드 하위 단계의 요청별 시작·끝을 일관되게 연결하지 못했으므로 Framework 전용 sub-segment의 별도 p50/p90 또는 CPU/대기 분해로 해석할 수 없다.

## 경계와 표본

x86-64 syscall 번호 44/45에서 양쪽 프로세스의 데이터 syscall을 길이·반환값·FD로 확인했다. Framework는 source `sendto(fd=ad, len=0x591, ret=1425)` → target `recvfrom(fd=e0, ret=1425)` → target `sendto(fd=f1, len=0x56e, ret=1390)` → source `recvfrom(fd=b1, ret=1390)` 순서다. Binding은 source `sendto(fd=4c, len=0x4e4, ret=1252)` → target `recvfrom(fd=c5, ret=1252)` → target `sendto(fd=e3, len=0x4e4, ret=1252)` → source `recvfrom(fd=53, ret=1252)` 순서다. `EAGAIN`과 0바이트 반환은 제외했다.

완전한 순서 `S_tx → T_rx → T_tx → S_rx → 다음 S_tx`로 맞은 표본은 Framework 5,313 cycle, Binding 8,259 cycle이다. 중간에서 순서가 끊겨 버린 cycle은 각각 224개와 37개다. 프로세스별 IO thread는 Framework source 송신 TID 2912139, target 수신 TID 2912010, target 송신 TID 2912011, source 수신 TID 2912141이다. Binding은 각각 TID 2914343, 2914263, 2914264, 2914345다.

완전한 cycle 표본의 A/B/C/D 평균은 Framework 12.0/114.1/22.0/135.0µs, Binding 21.0/57.8/21.3/81.5µs다. 합계 평균은 Framework 283.0µs, Binding 181.6µs다.

## 3초 처리량과 대조

`window.json`의 source completed 증가량은 Framework 23,760건, Binding 37,827건이다. 3초로 나눈 처리량 역수는 각각 126.3µs와 79.3µs다. cycle에서 얻은 283.0µs와 181.6µs는 이 역수와 맞지 않는다. 완전 cycle의 시각 범위는 각각 3.008초와 3.004초로 전체 perf 창에 걸쳐 있으므로, 이 차이는 캡처 구간이 더 짧아서 생긴 것으로 설명되지 않는다.

데이터에는 `PERF_RECORD_LOST`도 있다. Framework 기록은 1,973개, Binding 기록은 2,078개와 1,246개 유실을 보고한다. syscall 후보 호출 수에 비해 완전 순서 cycle이 Framework 약 절반, Binding 약 절반만 구성된다. 따라서 표의 p50/p90은 순서가 보존된 표본의 수치이며, 전체 3초 요청의 p50/p90으로 확정할 수 없다.

순서가 끊기는 구간의 실제 syscall 발췌는 다음과 같다. 이 구간에서는 source reply 수신이 새 source 송신 직후에 관측되고, target reply 송신도 target request 수신보다 먼저 관측된다. 이 호출들을 한 요청으로 연결할 수 없다.

```text
1988162.309192456 source TID 2912139 sendto fd=ad ret=1425
1988162.309298335 source TID 2912141 recvfrom fd=b1 ret=1390
1988162.311871325 target TID 2912011 sendto fd=f1 ret=1390
1988162.312282528 target TID 2912010 recvfrom fd=e0 ret=1425
1988162.312382416 target TID 2912011 sendto fd=f1 ret=1390
1988162.312413615 source TID 2912141 recvfrom fd=b1 ret=1390
```

이후 `1988162.312276687 → 1988162.312282528 → 1988162.312382416 → 1988162.312413615 → 1988162.312523832` 구간은 syscall 경계 순서와 길이가 맞아 cycle 하나로 계산했다. 하지만 전체 syscall stream의 유실과 위와 같은 역순 호출이 있어 요청별 scheduler handoff를 전부 정렬하지 못했다. `sched_wakeup`의 반복 이벤트를 특정 cycle에 붙이고 해당 thread의 switch-in 및 다음 mailbox/eventfd/futex syscall까지 잇는 데 필요한 유일한 handoff가 캡처에서 안정적으로 나타나지 않았다. 따라서 B와 D의 하위 구간 p50/p90, 하위 구간별 on-CPU 및 runnable/blocked 대기 시간은 산출하지 않았다.

## 완료 항목

코드 변경 없음. 리팩토링 점검: 코드 변경 없음.
