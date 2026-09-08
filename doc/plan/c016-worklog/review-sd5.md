# review-SD-5 — B-SD4-1 해소 최종 재검증

검토 기준은 base `84d25131a6`과 고정 artifact
`/home/hep7hep7/project/zlink-work/all-artifacts/SD-5.patch`
(SHA-256 `07a9bbc9b48c515b772368797a01f49cfb50507df7875a55fc7759376faa61d7`)다.
`review-sd4.md`, `core-rf-SD-5-report.md`, ZMP §9와 보존된 WS gate report를 함께 읽었다.
소스 worktree는 읽지 않았고 소스·스펙 수정, 빌드, 실행, 커밋을 하지 않았다. 작성한 파일은
이 보고서와 `progress-review-sd5.md`뿐이다. B는 채택 차단, W는 비차단 경고·증거 한계,
S는 확인 결과다.

## 결론

**B-SD4-1은 해소됐다.** 64 KiB body를 pointer로 보관한 뒤에도 `prepared_size`가 target에
도달할 때까지 즉시 준비된 frame을 encoder buffer 뒤쪽에 계속 넣는다. 제출할 때는 split
offset을 기준으로 `[앞부분, body, 뒷부분]`을 만들고 `async_writev()` 한 번에 넘기며, WS/WSS
공통 구현은 이 sequence 전체를 Beast `async_write()` 한 번에 넘긴다
(`asio_engine.cpp:1313-1433,794-837`, `ws_transport_common_internal.hpp:237-259`).

앞+body+뒤 합계는 `prepared_size`로 target까지 제한하고 pointer admission은 128 KiB max도
검사한다. 두 번째 pointer body는 현재 batch에 넣지 않고 같은 `gather_split_offset`의 예약
비트로 다음 batch에 넘긴다(`asio_engine.cpp:1330-1343,1392-1429`,
`ws_batch_policy.hpp:28-48`). 네 반례도 pure policy test에 머물지 않고 실제 ZMP encoder와
fake message-boundary transport를 통과해 write 횟수, buffer 수, wire frame 순서를 검사한다
(`unittest_zmp_engine_controls.cpp:100-185`, `contract_zmp_engine_fixture.hpp:202-238`).

신규 차단 결함은 찾지 못했다. 다만 r3의 endpoint 실패는 원문이 잘린 monitor snapshot 한 줄만
남아 있어 원인을 확정할 수 없고, SD-5 TSan·hotpath 원로그도 고정 artifact에 없다. 두 항목은
아래 W-SD5-1/2로 남긴다.

## 항목별 판정

| 항목 | 판정 | 근거 파일:행과 판단 |
|---|---|---|
| `64 KiB body → small`을 target까지 계속 수집 | **통과 — S-SD5-1** | 첫 body를 별도 `msg_t`로 옮기고 split을 표시한 뒤 `continue`한다. 다음 `_next_msg`가 즉시 실패할 때나 `prepared_size == target`일 때만 loop를 끝낸다(`asio_engine.cpp:1320-1328,1360-1399,1402-1429`). 따라서 SD-4의 pointer 발견 즉시 제출 경로가 없어졌다. |
| 앞+body+뒤 target/max 상한 | **통과** | Pointer admission은 기존 copy byte와 header가 target/max에 들어가는지 먼저 확인하고, body까지의 합계를 max와 비교한다(`ws_batch_policy.hpp:28-41`). 이후 `prepared_size = copy + body`를 유지하고 남은 `target_out_batch - prepared_size`만 encoder에 준다(`asio_engine.cpp:1392-1405,1417-1429`). 합계가 max를 넘는 body는 pointer 경로에 들어가지 않고 기존 encoder 분할로 간다. |
| 한 batch의 pointer body 최대 1개 | **통과** | `_pipeline.async_gather` 상태에서 두 번째 threshold body를 만나면 encode하지 않고 pending 비트만 세워 현재 loop를 닫는다(`asio_engine.cpp:1330-1334`). Completion은 pending 비트만 보존하고 다음 `prepare_output_buffer()`가 `_pipeline.tx_msg`의 그 body를 먼저 처리한다(`:845-863,1313-1328`). 별도 queue나 두 번째 body pointer는 없다. |
| 제출 횟수와 순서 | **통과** | Split이 없거나 앞/뒤가 비어 있는 경우까지 2~3개 buffer를 순서대로 구성하고 transport `async_writev()`를 정확히 한 번 호출한다(`asio_engine.cpp:794-837`). WS/WSS는 sequence를 복사한 뒤 payload 합계를 정하고 Beast `async_write()`를 한 번 호출한다(`ws_transport_common_internal.hpp:237-259`; adapter는 `ws_transport.cpp:127-138`, `wss_transport.cpp:137-148`). |
| 네 반례의 실제 encoder test | **통과 — S-SD5-2** | Fixture는 실제 engine/session에 queue를 공급하고 fake transport가 각 `async_write_some/writev`의 byte와 buffer 수를 기록한다(`contract_zmp_engine_fixture.hpp:92-99,202-238,249-310`). Test는 `64 KiB→small` 1회/3 buffers, `small→64 KiB→small` 1회/3 buffers, max 초과 2회/각 1 buffer, pointer 2개 2회/각 2 buffers를 요구하며 decode한 frame 크기·순서도 검사한다(`unittest_zmp_engine_controls.cpp:100-185`). SD-4의 “policy 반환값만 검사” 문제를 해소한다. |
| Pointer body 수명 | **통과 / W-SD5-3** | Bounded body를 heap `msg_t`로 옮기고 그 `shared_ptr`를 completion lambda가 값으로 잡으므로 callback 종료 전까지 underlying byte가 유지된다(`asio_engine.cpp:99-106,1344-1357,1384-1389,830-837`). 앞/뒤 buffer는 같은 engine의 encoder buffer이며 `write_pending` 동안 재사용하지 않는다(`:671-680,803-804`). 다만 encoder test의 fake transport는 제출 시 byte를 즉시 복사하므로 지연 소비 자체를 검증하지는 않는다. 코드상 수명은 성립하지만 3-buffer pending/cancel 전용 test는 없다. |
| 완료 회계·부분 write·오류 | **통과** | Buffer sequence가 앞→body→뒤 순서를 정하고 Beast composed write가 전체 sequence의 최종 completion 하나를 반환한다(`asio_engine.cpp:808-837`, `ws_transport_common_internal.hpp:250-259`). Completion 뒤 `finish_gather_output()`은 gather pointer와 copy byte를 함께 비우고, 성공 뒤 pending body가 있으면 그 표식만 다음 batch에 남긴다(`asio_engine.cpp:840-863`). SD-4에서 확인한 non-aborted 오류·partial write의 connection error 처리와 callback guard 계약은 바뀌지 않았다(`review-sd4.md:34-35`). 오류 후 같은 WS message 일부를 재제출하는 새 경로는 없다. |
| 취소·disconnect·ctx term의 split 정리 | **통과 / W-SD5-3** | 정상 completion은 split을 0 또는 pending 비트 하나로 정규화한다(`asio_engine.cpp:845-856`). 취소/종료로 callback guard가 만료되면 handler capture가 해제되어 bounded body도 반납되고, pipeline 자체가 engine별로 파괴되므로 split이 다음 connection으로 전달되지 않는다(`:806-837`, `asio_engine_pipeline.hpp:27-48,60-79`). 기존 guard 무효화→transport close→지연 파괴 순서는 patch에서 바뀌지 않았다(`review-sd4.md:35`). 전용 취소 test가 없는 점은 W-SD5-3의 증거 한계다. |
| `async_writev` 일반화와 TCP/TLS/IPC 정합 | **통과** | 내부 interface는 `const_buffer* + count` 하나로 바뀌었고(`i_asio_transport.hpp:143-158`), TCP/IPC/WS/WSS와 fake transport가 같은 signature를 구현한다. TCP는 3개 이상을 portable Asio sequence로, 기존 2개는 native 경로로 유지한다(`tcp_transport.cpp:583-642`). IPC도 Windows와 3개 이상은 Asio, Unix 2개는 기존 native 경로다(`ipc_transport.cpp:470-551`). WS/WSS의 TLS 여부와 관계없이 공통 helper가 sequence 전체를 Beast에 넘긴다. Patch의 실제 production 호출자는 engine 한 곳뿐이라 중복 조립도 없다. |
| TCP ZMP·RAW STREAM 불변 | **통과 — S-SD5-3** | Bounded pointer branch는 ZMP message-boundary transport와 gather 지원을 동시에 요구한다(`asio_engine.cpp:1336-1343`). TCP ZMP는 기존 env opt-in gather만 가능하고 TCP는 message-boundary가 아니므로 새 3-buffer branch에 들어가지 않는다. RAW STREAM은 gather header를 만들지 않아 기존 byte encoder 경로를 쓴다. 이 조건은 SD-4에서 확인한 행렬과 같다(`review-sd4.md:28-29,37`). `async_writev`의 기존 1/2-buffer 순서도 보존된다. |
| Bounded batch의 대기 없음 | **통과** | Loop는 `_next_msg`가 지금 message를 주는 동안에만 계속하며 `EAGAIN`이면 바로 끝난다(`asio_engine.cpp:1320-1327`). Timer, retry, future traffic 대기 상태가 추가되지 않았다. |
| 새 영속 상태와 기존 B2·D-f·app | **통과** | 새 pipeline field는 `size_t gather_split_offset` 하나다(`asio_engine_pipeline.hpp:38-41,68-72`). Pending 여부는 그 값의 high bit에 합쳐 새 bool/queue를 만들지 않았다(`asio_engine.cpp:94-97,1330-1334`). App target blob은 SD-4와 같은 `c4bceac7d3`이고, patch의 B2 one-hit 성장과 D-f의 사용하지 않는 STREAM gather env accessor 제거도 SD-4에서 확인한 형태 그대로다(`review-sd4.md:39,41`). SD-5의 추가 동작은 bounded split/제출과 그 test에 한정된다. |
| WS gate r1/r2 | **통과** | 보존 report는 시작 load1 1.31/3.78에서 각각 18/18이다. r1/r2의 ws Q64/Q1은 0.967902/0.830596, wss는 0.832113/1.358819로 모두 0.80 이상이다(`core-rf-SD-5-report.md:48-56`; 원자료 r1/r2). |
| WS gate r3 endpoint 실패 | **비차단 경고 — W-SD5-1** | r3는 load1 3.99에서 17/18이며 `MULTI_ROUTER_ROUTER_SENDSEND/ws/65536`만 실패했다. 보존 report는 실패를 client `endpoint`, `monitor_snapshot`으로 분류하지만 상세 문자열이 `...,ro`에서 잘린다(`SD-5-ws-gate-r3/...txt:141-155,290-300`). 같은 cell이 r1/r2에서 통과했고 r3의 WS 1 KiB와 WSS 64 KiB도 통과했다. 코드상 split은 engine별 `_pipeline` field이고 transport도 connection별 객체라 connection 간 공유가 없다(`asio_engine_pipeline.hpp:26-80`, `ws_transport.cpp:127-138`). 뒤쪽 frame도 같은 engine의 `_next_msg`에서만 꺼내므로 다른 RID/peer로 섞는 새 전역 경로가 없다. 따라서 새 3-buffer 결함을 가리키는 직접 증거는 없지만, 잘린 endpoint 기록만으로 외부 setup 오류라고 확정할 수도 없다. |
| r3 재현·확인 방법 | **필요 — W-SD5-1** | 동일 조건(100 clients, ROUTER↔ROUTER SENDSEND, WS, 65,536 B, server/client I/O thread 4, auto-HWM, 5 s)을 단일 cell로 격리한다. 첫 재현부터 기존 message-flow/file log와 run dir를 보존해 어느 connection의 READY/DISCONNECTED 또는 protocol error가 먼저인지 `flow`·`corr`로 잇는다. 동시에 peer별 송신 frame의 RID/sequence와 `[앞,body,뒤]` byte를 비교한다. 별도 contract test로 두 message-boundary engine을 번갈아 진행하고 각 engine의 write를 따로 decode하면 split의 connection 격리와 다른 RID 혼입 여부를 직접 확인할 수 있다. Pending 3-buffer write 도중 cancel/disconnect하는 test도 W-SD5-3을 함께 닫는다. |
| Hotpath 5/5 | **통과 / W-SD5-2** | reqrep reference `15609.7872`에서 측정 `15624.6704`(1.0010)이며 나머지 네 cell도 0.9611~1.0461로 5/5다(`core-rf-SD-5-report.md:64-74`). Patch에 `hotpath_reference.json` diff는 없다. 다만 이 수치의 별도 원로그는 고정 artifact에 없어 보고서와 patch 내 보고서 사본까지만 교차 확인했다. |
| TSan 27-suite | **보고 통과 / W-SD5-2** | 결과 보고서는 GCC TSan 27/27, 22.39 s, sanitizer 보고 0건이라고 기록한다(`core-rf-SD-5-report.md:34-46`). 그러나 SD-5 이름의 TSan 원로그는 고정 artifact에 없고, 동시 사용 중인 worktree를 읽지 말라는 제한 때문에 `LastTest.log`를 독립 확인하지 않았다. 따라서 실행 결과를 반박할 근거는 없지만 “로그 직접 확인” 증거는 미충족이다. |
| Public header·version script·platform API | **통과 / W-SD5-4** | 23개 diff 중 `core/include/**`와 `core/src/libzlink.vers`는 없다. 바뀐 `i_asio_transport`는 내부 header다. Windows TCP/IPC는 3-buffer를 Asio sequence로 처리하고 WS/WSS는 공통 Beast API를 쓰며, macOS를 포함한 Unix의 기존 2-buffer native 경로는 유지된다(`tcp_transport.cpp:583-642`, `ipc_transport.cpp:470-551`, `ws_transport_common_internal.hpp:237-259`). Public API/ABI 변화는 없다. 다만 보존된 Windows/macOS compile 결과는 입력에 없다. |

## r3 오류의 3-buffer 연관성 판단

가능성은 **낮지만 미확정**이다. 새 상태는 각 `asio_engine_t`가 가진 pipeline 안에 있고
(`asio_engine_pipeline.hpp:26-80`), WS adapter가 잡는 connection도 instance별
`_connection`이다(`ws_transport.cpp:127-138`). 첫 pointer body 뒤의 frame도 같은 engine/session의
`_next_msg`에서만 가져온다. 따라서 “한 ROUTER의 전역 split 상태가 다른 peer로 넘어간다”는
가설에 맞는 공유 상태나 lookup이 patch에 없다. Buffer 순서도 split offset 하나에서 직접 만들어져
RID별 재분배를 하지 않는다(`asio_engine.cpp:794-831`).

반대로 r3 report는 실제 endpoint 오류의 errno, event와 peer를 보존하지 않았고 실패 문자열도
잘렸다. 잘못된 byte 때문에 peer가 disconnect한 뒤 endpoint 부족으로 보였을 가능성까지 이 파일만으로
배제할 수는 없다. 그러므로 전체 gate를 반복하기보다 위 표의 단일 cell 재현에서 첫
DISCONNECTED/protocol error를 message flow로 잡고, peer별 wire decode와 split 격리 test로 확인해야
한다.

## 신규 B/W/S

| ID | 등급 | 내용 |
|---|---|---|
| W-SD5-1 | **W** | r3의 ROUTER↔ROUTER WS 64 KiB endpoint 실패는 상세 원로그가 잘려 원인 미확정이다. Patch에는 split/RID가 connection 사이에 공유되는 경로가 없어 3-buffer 결함 가능성은 낮지만, 단일 cell flow·peer별 wire 확인 전에는 외부 오류로 단정할 수 없다. |
| W-SD5-2 | **W** | Hotpath 5/5와 TSan 27/27·0건은 결과 보고서에서 확인했지만 해당 SD-5 원로그가 고정 artifact에 없다. 특히 TSan “로그 직접 확인”은 입력 제한상 완료하지 못했다. |
| W-SD5-3 | **W** | Body 소유권 capture와 종료 설계는 타당하지만 fake encoder test가 byte를 제출 즉시 복사한다. 3-buffer pending 상태의 지연 소비와 cancel/disconnect를 직접 고정하는 test는 없다. |
| W-SD5-4 | **W** | 내부 API의 Windows/macOS 분기는 코드상 정합하지만 해당 플랫폼 compile log는 없다. |
| S-SD5-1 | **S** | B-SD4-1의 원 반례에서 64 KiB pointer body 뒤의 준비 frame을 target까지 모아 한 3-buffer write로 제출한다. |
| S-SD5-2 | **S** | 네 반례가 실제 encoder/fake transport를 지나 write 횟수·buffer 수·wire frame 순서를 검사한다. |
| S-SD5-3 | **S** | TCP ZMP env opt-in과 RAW STREAM byte 경로, bounded batch의 대기 없음, B2·D-f·app, public API/ABI와 hotpath reference가 유지된다. |

## 소유 계층·스펙·교차언어·분류

- 소유 계층: 준비된 ZMP byte의 target, pointer split과 다음 batch pending은 Core Asio engine이
  소유한다. Transport는 받은 buffer sequence를 한 operation으로 제출할 뿐 batch나 RID를 다시
  판단하지 않는다.
- 스펙 조항: `01-zmp.ko.md:489-492`는 현재 준비된 byte를 `out_batch_size`까지 모아 bounded
  batch 하나를 Beast write 한 번으로 제출하고, 아직 없는 traffic은 기다리지 않으며 frame byte와
  multipart 경계를 바꾸지 않도록 요구한다. Patch는 이 네 문장을 모두 유지한다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin binding은 같은 native Core engine/transport를 사용하므로
  언어별 보상 구현이 없다. 한 언어만 달라지는 runtime 동작도 없다.
- 변경 분류: **B — SD-4 bounded batching의 기존 결함 수정.** 계약을 바꾸거나 상위 계층에서
  보상하지 않고 batch를 소유한 engine과 sequence를 소유한 transport에서 고쳤다.
- 규칙 수: 수정 전에는 “pointer body면 즉시 닫기”와 “나머지를 다음 write로 보내기”가 있었고,
  수정 후에는 “준비된 합계를 target/max까지 한 bounded batch로 보내되 pointer는 하나만 둔다”는
  규칙 하나다. 새 영속 상태는 split offset 하나다.

차단 항목 수 / 채택 가능 여부: 0 / 채택 가능(W-SD5-1~4 후속 확인 권고)
