# .NET binding performance pass 3

결과: 부분 완료. 공개 계약을 유지하는 할당·수신 경로 개선과 completion 재제출 결함 수정은 구현했다. 즉시 제출 무락과 지속 단일 pump는 검증 회귀 때문에 제출하지 않았다. 성능 목표 달성 및 작은 메시지 전체 격차의 인과 확정은 완료하지 못했다.

## 설계 비교

|항목|.NET before|C++ 비교|Java 1b 비교|최종 .NET|
|---|---|---|---|---|
|제출·drain 락|submit 공통 락 + drain 중복 락|즉시 native 제출 뒤 토큰 등록; early completion replay|socket drain/lane + Context 진행|submit 락 유지, drain 중복 락 제거|
|완료 진행|socket pending 발생 시 Task.Run; idle 종료|completion owner와 async state bundle|Context당 native poller/control pair/진행 thread 1개|원래 pump 유지|
|REQREP 상태|entry + 별도 TCS + lock object + settlement flag|operation/resume state bundle|별도 pending 상태|entry가 TCS 상속; Task.IsCompleted와 registry가 단일 사실 소유|
|수신|중간 native 배열에서 최종 Message로 재이동|직접 native ABI|JNI 경계|최종 Message 배열로 직접 이동; private 배열만 재사용|
|P/Invoke|2-part REQREP 왕복 45회|해당 없음: 직접 ABI|해당 없음: JNI|같은 계측 41회|

검토 대안: (1) socket 지속 async pump, (2) Context 단일 poller/pump, (3) 기존 진행 모델을 유지하고 중복 상태·이동 제거. 1·2는 64KiB REQREP 처리량 급락 또는 drain timeout을 보였으므로 3을 선택했다. 무락 multipart 제출은 동시 제출 테스트에서 EINVAL이 발생했다. MORE→FINAL이 socket-local staging인 현재 계약에서는 part 호출의 개별 동시성만으로 record 원자성이 보장되지 않는다. 상위에 별도 generation/재시도 제한/timeout 증가를 추가하지 않았다.

## 변경 파일

- `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`: Request entry/TCS 통합, 중복 drain lock·제거 상태·settlement 상태 제거. WRITABLE 처리 시 즉시 재제출하지 않고 native completion을 NO_DATA까지 소비한 뒤 기존 entry를 재제출한다.
- `bindings/dotnet/src/Zlink/Runtime/Messaging/MultipartMessageCollection.cs`: 공개 collection identity를 유지하면서 private Message[]만 ArrayPool로 재사용한다. dispose 후 enumerator 접근을 차단하고 소유권 이전 시 독립 배열을 반환한다.
- `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs`: 중간 native vector를 없애고 최종 Message wrapper로 직접 이동한다. 실패 경로는 생성된 wrapper를 정리하고 배열을 반환한다.
- `bindings/dotnet/tests/Zlink.Tests/test_hot_path_ownership_contract.cs`: 배열 재사용 이후 disposed collection/enumerator, request Task/result identity, 동시 public drain/backpressure 제출 검증 추가. 기존 assertion은 변경하지 않았다.

수정 전/후 규칙 수: registry membership·request settlement·drain 배제·수신 native 이동의 네 항목 기준 8→4. 기존 pump 생성 규칙은 남아 있다.

## 계측

instrumented assembly는 별도 artifact에서 빌드했으며 production 코드에 counter/log를 남기지 않았다. public completion 소유, 2-part inproc REQREP 20,000회 기준이다. benchmark 처리량과 다른 진단 workload이므로 시간 수치를 직접 합산하지 않는다.

|항목|before|after|
|---|---:|---:|
|request 호출 할당 B/op|288|232|
|reply 호출 할당 B/op|32|32|
|public drain 할당 B/op|40|40|
|왕복 전체 할당 B/op|640|544|
|P/Invoke/왕복|45|41|
|msg_init/왕복|12|10|
|msg_move/왕복|6|4|
|submit gate 진입/20,000왕복|60,000|60,000|
|submit gate 경합/20,000왕복|0|0|

할당은 15%, native 호출 수는 8.9% 감소했다. 이 workload에서는 락 경합이 원인이라는 가설을 지지하지 않는다. 원래 runtime probe에서는 1,000회에 pump 1,000회, 경합 5회/합계 150,878ns를 관측했다. 최종 runtime probe는 pump=0으로 나와 runtime 진행 비교 근거로 사용하지 않는다. P/Invoke 호출 수는 확정했으나 개별 native 호출 비용이나 작은 메시지 2.5배 격차 전체를 분해·확정하지는 못했다.

## Gate

- `bindings/dotnet/tests/run_tests.sh`: 전체 222/222 PASS; samples 7/7 PASS (`gate-submitted.log`).
- ownership/backpressure/completion 관련 필터: 43개 × 5회 PASS (`submitted-repeat-{1..5}.log`).
- 공개 API reflection surface 1,439줄: before/after diff 0 (`api.diff`).
- `git diff --check`: PASS.
- core/build·core/build-dev 빌드/clean 없음. core/spec/doc/다른 binding/러너 수정 없음. commit/push/checkout/reset/stash 없음.

## 계약과 spec gap

소유 계층: native admission/readiness/correlation은 Core, managed Task·wrapper·completion 진행은 binding.
Spec 근거: `core/doc/spec/core/socket/README.ko.md`의 multipart MORE/FINAL 계약(931 부근), completion NO_DATA까지 drain 후 재시도(1068 부근); `bindings/doc/spec/async-execution-model.ko.md:62–80`의 단일 native drain owner. 이 문서들은 읽기만 했다.
교차언어: C++ entry/resume bundle과 Java Context pump를 대조했다. C++/Java의 WRITABLE capture에도 inline retry가 있어 NO_DATA 경계 점검이 필요하다. 수정은 .NET에만 했다. managed TCS/Message[] 개선은 .NET 구조에 해당한다.
분류: B 기존 결함(NO_DATA 이전 재제출), A 기존 공개 계약을 유지하는 내부 표현·소유권 정리. Framework runtime 변경은 없다.
Spec gap: 새 공개 계약은 필요하지 않았다. 다만 아래 native readiness 관측의 정확한 분류는 미해결이며 binding 우회로 숨기지 않았다.

공개 C API repro는 첫 64KiB 요청을 router가 받은 뒤 reply하지 않은 상태에서 두 번째 요청이 EAGAIN/WRITABLE을 1,000회 연속 즉시 반복했다(3.356ms, competing sender 없음). `repro_request_writable.c`와 log를 보존했다. 이것만으로 최초 성능 격차의 단일 원인을 확정하지 않는다. 현재 source의 correlation-budget errno와 실행 binary의 관측 errno가 달라 source/binary 대응도 추가 확인이 필요하다. 초기 runtime SHA256은 `543e1089430176bf861f9ef8b7974941e3d785dee8d93bb8d4a39d62e1d08538` (mtime 15:15:18)이었다. 종료 확인에서는 공유 symlink 대상이 `e680b264822a92f770769a37ab9df152b342413189cfb4b148404c1f5ed9b4ea` (mtime 16:51:35, 6,509,240 bytes)로 바뀌어 있었다. 이 작업에서는 Core를 빌드하거나 수정하지 않았다. 따라서 초기 계측·후기 gate/after 사이의 native artifact 동일성을 주장할 수 없다. 이것은 공식 비교를 보류하는 추가 blocker다.

## BLOCKERS

1. 즉시 성공 경로 완전 무락·무할당 및 backpressure마다 생성하지 않는 단일 pump는 미완료. 후보의 계약·성능 회귀를 제거한 설계가 더 필요하다.
2. 작은 메시지 성능 격차의 전체 비용 분해와 목표 달성은 미확정. 단순 alloc 감소만으로 원인을 단정할 수 없다.
3. Context 후보는 readiness 반복과 poller event capacity 문제를 보였다. 동일 현재 Core에서 원본 DLL의 64KiB DR/RR는 25,836/22,955 ops/s로 정상이다. 후보 회귀를 Core artifact 차이만으로 설명할 수 없다.
4. 공식 after load≤3 조건은 시작 시 확인했다. 실행 중 load가 3을 넘으면 해당 결과는 진단값으로만 취급하며 공식 판정은 보류한다. 다른 job의 실행 여부도 시작 시 프로세스로 확인했다. 사용자에게 요청한 시작 시/전 구간 조건 해석은 회신이 없어 완화하지 않았다.

## 보고서와 증거

최종 after 표는 아래에 추가한다. 폐기 후보의 첫 after 보고서는 `reports/perf_dotnet_multi_linux_20260905_161551.txt`로 보존하며 최종 결과와 혼합하지 않는다. 진단 코드·원본 DLL·전체 gate/probe log는 `reports/dotnet-pass3-profile/`에 보존한다.

## 최종 after 비교 (진단값; 공식 판정 보류)

요청한 명령 그대로 100 clients, TCP, 5초, 1 run, 20/20 cells 완료. 17:09:36 시작 load=1.75, 다른 측정 프로세스 없음. 실행 중 load>3을 관측했다. 이전 baseline은 paired 3-run, after는 1-run이므로 변동성도 다르다.

최종 보고서: `reports/perf_dotnet_multi_linux_20260905_170936.txt`.

|패턴|bytes|before ops/s|after ops/s|변화|C ops/s|after/C|
|---|---:|---:|---:|---:|---:|---:|
|DEALER_DEALER|64|413,135.6|462,415.8|+11.9%|992,291.2|46.6%|
|DEALER_DEALER|256|392,055.2|412,496.6|+5.2%|950,890.0|43.4%|
|DEALER_DEALER|1024|401,174.2|427,591.2|+6.6%|866,574.2|49.3%|
|DEALER_DEALER|4096|248,693.2|278,768.2|+12.1%|353,103.0|78.9%|
|DEALER_DEALER|65536|69,442.8|80,543.6|+16.0%|65,463.6|123.0%|
|DEALER_ROUTER_REQREP|64|70,865.4|69,587.6|-1.8%|200,303.6|34.7%|
|DEALER_ROUTER_REQREP|256|69,571.0|64,892.2|-6.7%|171,696.0|37.8%|
|DEALER_ROUTER_REQREP|1024|70,721.2|71,527.2|+1.1%|143,316.4|49.9%|
|DEALER_ROUTER_REQREP|4096|68,467.8|60,218.2|-12.0%|125,960.8|47.8%|
|DEALER_ROUTER_REQREP|65536|26,036.0|28,129.6|+8.0%|23,034.8|122.1%|
|ROUTER_ROUTER_REQREP|64|67,998.2|68,963.6|+1.4%|168,422.4|40.9%|
|ROUTER_ROUTER_REQREP|256|69,758.0|58,302.0|-16.4%|123,456.6|47.2%|
|ROUTER_ROUTER_REQREP|1024|67,376.4|69,851.6|+3.7%|121,064.8|57.7%|
|ROUTER_ROUTER_REQREP|4096|65,230.4|62,869.6|-3.6%|103,400.8|60.8%|
|ROUTER_ROUTER_REQREP|65536|24,785.2|26,327.4|+6.2%|19,895.8|132.3%|
|PUBSUB|64|340,321.6|518,345.6|+52.3%|778,303.0|66.6%|
|PUBSUB|256|349,110.4|556,468.2|+59.4%|707,142.4|78.7%|
|PUBSUB|1024|432,211.2|545,701.4|+26.3%|806,768.2|67.6%|
|PUBSUB|4096|381,097.0|498,678.4|+30.9%|588,467.0|84.7%|
|PUBSUB|65536|46,169.2|51,048.8|+10.6%|63,144.2|80.8%|

|패턴|before/C 평균|after/C 평균|목표|
|---|---:|---:|---:|
|DEALER_DEALER|61.1%|68.3%|85%|
|DEALER_ROUTER_REQREP|58.5%|58.5%|70%|
|ROUTER_ROUTER_REQREP|68.0%|67.8%|70%|
|PUBSUB|56.9%|75.7%|85%|

|패턴|bytes|before mean ms|after mean ms|before p95 ms|after p95 ms|
|---|---:|---:|---:|---:|---:|
|DEALER_DEALER|64|0.597|1.992|6.480|22.378|
|DEALER_DEALER|256|0.496|0.460|5.477|1.638|
|DEALER_DEALER|1024|536.193|540.968|1934.744|1567.329|
|DEALER_DEALER|4096|393.027|300.473|909.716|715.125|
|DEALER_DEALER|65536|7.509|4.809|25.287|9.447|
|DEALER_ROUTER_REQREP|64|0.452|0.467|0.762|0.788|
|DEALER_ROUTER_REQREP|256|0.451|0.454|0.769|0.777|
|DEALER_ROUTER_REQREP|1024|0.466|0.458|0.792|0.779|
|DEALER_ROUTER_REQREP|4096|0.498|0.478|0.848|0.829|
|DEALER_ROUTER_REQREP|65536|1.660|1.564|3.148|2.937|
|ROUTER_ROUTER_REQREP|64|0.480|0.449|0.829|0.773|
|ROUTER_ROUTER_REQREP|256|0.488|0.490|0.856|0.877|
|ROUTER_ROUTER_REQREP|1024|0.488|0.482|0.826|0.808|
|ROUTER_ROUTER_REQREP|4096|0.518|0.510|0.849|0.866|
|ROUTER_ROUTER_REQREP|65536|1.646|1.497|2.460|2.370|
|PUBSUB|64|1380.923|1615.442|3711.878|3888.268|
|PUBSUB|256|1820.921|1881.966|3987.197|4082.225|
|PUBSUB|1024|1670.577|1442.781|3379.402|3317.006|
|PUBSUB|4096|620.299|530.645|1037.258|1040.795|
|PUBSUB|65536|321.907|295.371|668.500|491.652|

Baseline 입력 보고서:

- `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_145518.txt`
- `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_145814.txt`
- `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_150212.txt`
- `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_150611.txt`
- `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_145351_p1dotnet-r3q.txt`
- `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_145648_p1dotnet-r3q.txt`
- `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_150046_p1dotnet-r3q.txt`
- `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_150445_p1dotnet-r3q.txt`
