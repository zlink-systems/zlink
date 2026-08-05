# 2026-06-01 Node bindings 성능 작업 로그

이 문서는 `bindings-library-performance-improvement-plan-2026-05-30.ko.md`에서 분리한
Node 측정과 후보 검토 기록이다. 계획 문서 본문에는 최종 상태 표와 간단한 판정만 남긴다.

## 진행 원칙

- 후보 개발 중에는 남은 보류 항목의 pattern, transport, msg-size만 좁혀 측정한다.
- 후보가 표에 반영될 만큼 개선되면 같은 failset 또는 all-transport 묶음으로 다시 확인한다.
- 다른 언어로 넘어가기 전에는 Node 전체 또는 문서 failset을 넓게 재확인한다.
- `bindings/node/dist/index.d.ts`와 `bindings/node/src/zlink/contracts/**` 공개 계약은 바꾸지 않는다.
- `recvPayloadInto`, `subscribePayloadInto`, `publishFrom`, `sendFrom`, borrowed send helper 계열은
  현재 public surface guard가 금지하므로 되살리지 않는다.

## 현재 문서 반영 방식

- 계획 문서 본문에는 결과 표, 기준 파일, 최종 판정만 둔다.
- 후보별 상세 측정, 실패 이유, 원복 근거는 이 log 파일에 이어서 남긴다.
- 이전에 계획 문서 `## 10. 라운드 진행 로그`에 들어 있던 시간순 로그는 본문에서 제거한다.
  필요한 세부 근거는 각 perf 결과 파일과 이 log 파일에 남긴다.

## 2026-06-01 정책 정정

- perf runner 전용 private raw receive/publish 후보와 HWM profile/floor 변경 후보는 최종 채택
  대상에서 제외한다.
- 해당 후보들은 아래 로그에 시도 기록으로만 남기며, 계획 문서 표의 최종 근거로 사용하지 않는다.
- 최종 채택 대상은 public contract를 바꾸지 않고, C perf와 같은 의미를 유지하는 bindings
  라이브러리 내부 변경으로 한정한다.

## 이전 log에서 재사용할 판단 기준

- 이전 log `2026-05-18-bindings-performance-round.ko.md`에서 Node에 효과가 컸던 항목은
  `MULTI_DEALER_DEALER`의 `sendFrom(..., DontWait)`와 `recvInto(buffer, DontWait)` 계열이었다.
  당시 tcp 전체가 통과권으로 올랐지만, 현재 public surface guard는 borrowed/raw payload helper
  계열을 금지하므로 그대로 되살리지 않는다.
- 같은 log에서 `MULTI_PUBSUB`은 `TopicMessage` 재사용만으로는 부족했고, 추가 개선은
  raw/typed subscribed receive facade가 필요하다고 정리되어 있었다. 현재 코드는
  `TopicMessage` 재사용과 latency sampling까지 반영되어 있으므로, 같은 재사용 후보를 반복하지 않는다.
- SPOT 계열의 과거 `MsgUnit(B)=4096` 문제는 context `autoHwmMsgUnitBytes` 공개 옵션으로
  해결된 상태다. 현재 perf 결과에서도 `MsgUnit(B)`가 msg size와 맞는지 먼저 확인하고,
  이 문제가 아닌 경우에는 send/recv hot path 병목으로 본다.
- `MULTI_STREAM` small은 과거에도 public stream send builder와 frame 재구성 비용이 병목으로
  기록됐다. Buffer 수명 안전성이 불명확한 scratch frame 재사용은 이번 라운드에서도 heap
  corruption을 만들었으므로 다시 시도하지 않는다.

## MULTI_SPOT_SENDSEND small active slot 32 후보

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 의도:
  - 100개 spot 전체를 active로 순회하는 비용을 줄이기 위해 small size active spot 수를 32로 제한한다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_135317_node_multi_spot_sendsend_small_active32_probe_20260601.txt`
  - status: complete
- 결과:
  - tcp 64/256/1024B: 32.142/36.066/29.075 Kops/s
  - tls 64/256/1024B: 31.786/32.085/24.265 Kops/s
  - ws 64/256/1024B: 35.248/23.292/28.920 Kops/s
  - wss 64/256/1024B: 21.766/26.160/21.986 Kops/s
- 판정:
  - 기존 최종값의 47~50 Kops/s대보다 낮은 cell이 많다.
  - active slot 축소는 순회 비용보다 round-trip concurrency 감소 효과가 더 컸다.
  - 보류 해소에 도움이 되지 않아 최종 코드와 표에는 반영하지 않는다.

## MULTI_ROUTER_ROUTER wss/tls 256/1024B 제한 재측정

- 대상:
  - `MULTI_ROUTER_ROUTER`
  - `wss,tls`
  - `256,1024B`
- 근거:
  - 이전 log에서 wss `MULTI_ROUTER_ROUTER 1024B`는 단독 재측정으로 통과한 전례가 있었다.
  - 현재 표에서도 wss/tls 256/1024B가 30% 기준 바로 아래라 넓은 full 대신 해당 cell만 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports wss,tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 256,1024 --duration 1 --runs 3 --results-tag node_multi_rr_wss_tls_256_1024_recheck_20260601`
  - Node: `perf_node_multi_linux_20260601_140048_node_multi_rr_wss_tls_256_1024_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - wss 256B: Node 112.980 Kops/s, C 375.848 Kops/s, C 대비 30.1%, 통과
  - wss 1024B: Node 111.327 Kops/s, C 356.472 Kops/s, C 대비 31.2%, 통과
  - tls 256B: Node 116.538 Kops/s, C 396.794 Kops/s, C 대비 29.4%, 보류
  - tls 1024B: Node 112.172 Kops/s, C 377.889 Kops/s, C 대비 29.7%, 보류
- 판정:
  - wss 256/1024B는 계획 문서 표에 통과로 반영한다.
  - tls 256/1024B는 기존 C full 기준으로는 30% 바로 아래였으므로 같은 조건 C 기준을 다시 확인한다.

## MULTI_ROUTER_ROUTER tls 256/1024B C 기준 제한 재측정

- 대상:
  - `MULTI_ROUTER_ROUTER`
  - `tls`
  - `256,1024B`
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --transports tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 256,1024 --duration 1 --runs 3 --results-tag node_rr_tls_256_1024_c_recheck_20260601`
  - C: `perf_c_multi_linux_20260601_140139_node_rr_tls_256_1024_c_recheck_20260601.txt`
  - Node: `perf_node_multi_linux_20260601_140048_node_multi_rr_wss_tls_256_1024_recheck_20260601.txt`
  - status: complete
- 결과:
  - tls 256B: Node 116.538 Kops/s, C 364.557 Kops/s, C 대비 32.0%, 통과
  - tls 1024B: Node 112.172 Kops/s, C 350.914 Kops/s, C 대비 32.0%, 통과
- 판정:
  - 기존 C full 기준에서는 30% 바로 아래였지만, 같은 조건 제한 C 기준으로는 둘 다 통과다.
  - 계획 문서 표에 tls 256/1024B를 통과로 반영한다.

## MULTI_DEALER_DEALER tcp large direct part 후보

- 대상:
  - `MULTI_DEALER_DEALER`
  - `tcp`
  - `65536,131072B`
- 의도:
  - 이전 log에서 효과가 컸던 `recvInto`는 현재 public surface guard가 금지하므로 되살리지 않는다.
  - 대신 현재 public `Received` storage를 유지한 채 perf server 내부의 `singlePartOrThrow()` 검증 호출만
    `received.parts[0]` 직접 접근으로 줄여 보았다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_140423_node_multi_dd_tcp_large_direct_part_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 65536B: 기존 Node 42.861 Kmsg/s, 후보 34.067 Kmsg/s, C 대비 23.6% -> 18.8%
  - 131072B: 기존 Node 23.041 Kmsg/s, 후보 22.719 Kmsg/s, C 대비 24.6% -> 24.3%
- 판정:
  - `singlePartOrThrow()` 검증 호출은 이 large 병목이 아니며, 65536B는 뚜렷하게 낮아졌다.
  - 최종 코드와 표에는 반영하지 않고 되돌린다.

## Single simple 64B 제한 재측정

- 대상:
  - `PAIR,PUBSUB,DEALER_DEALER`
  - `ws,wss,tls`
  - `64B`
- 근거:
  - 계획 문서에서 `PAIR tls 64B`, `PUBSUB ws/wss/tls 64B`, `DEALER_DEALER tls 64B`가
    35% 기준 바로 아래에 있었다.
  - 코드 후보보다 같은 조건 C/Node 제한 재측정으로 기준선 변동을 먼저 확인했다.
- 측정:
  - Node: `perf_node_single_linux_20260601_140751_node_single_simple64_recheck_20260601.txt`
  - C: `perf_c_single_linux_20260601_140755_node_single_simple64_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - `PAIR tls 64B`: Node 463.288 Kmsg/s, C 1245.184 Kmsg/s, C 대비 37.2%, 통과
  - `PUBSUB ws 64B`: Node 442.060 Kmsg/s, C 1235.177 Kmsg/s, C 대비 35.8%, 통과
  - `PUBSUB wss 64B`: Node 428.682 Kmsg/s, C 1235.792 Kmsg/s, C 대비 34.7%, 보류
  - `PUBSUB tls 64B`: Node 432.522 Kmsg/s, C 1227.706 Kmsg/s, C 대비 35.2%, 통과
  - `DEALER_DEALER tls 64B`: Node 447.989 Kmsg/s, C 1242.333 Kmsg/s, C 대비 36.1%, 통과
- 판정:
  - 4개 보류 cell은 제한 재측정으로 통과에 올랐으므로 계획 문서 표에 반영한다.
  - `PUBSUB wss 64B`는 34.7%로 여전히 기준에 못 닿아 보류를 유지한다.

## MULTI_PUBSUB tcp 낮은 보류 C 기준 제한 재측정

- 대상:
  - `MULTI_PUBSUB`
  - `tcp`
  - `64,256,65536,131072B`
- 근거:
  - 현재 표에서 `MULTI_PUBSUB tcp`는 13.9~22.2%대 낮은 보류가 남아 있었다.
  - Node hot path는 이미 `TopicMessage` 재사용, raw payload 기록, latency timestamp sampling을 적용했다.
    먼저 같은 조건 C 기준을 갱신해 기준선 영향을 분리한다.
- 측정:
  - C: `perf_c_multi_linux_20260601_141009_node_multi_pubsub_tcp_low_c_recheck_20260601.txt`
  - Node: `perf_node_multi_linux_20260601_095226_node_multi_pubsub_sample_timestamp_only_all_final_20260601.txt`
  - Node 1024B fill: `perf_node_multi_linux_20260601_095300_node_multi_pubsub_sample_timestamp_only_tcp1024_fill_20260601.txt`
  - status: complete
- 결과:
  - 64B: Node 547.986 Kmsg/s, C 2653.378 Kmsg/s, C 대비 20.7%, 보류
  - 256B: Node 529.894 Kmsg/s, C 2477.792 Kmsg/s, C 대비 21.4%, 보류
  - 65536B: Node 38.061 Kmsg/s, C 192.302 Kmsg/s, C 대비 19.8%, 보류
  - 131072B: Node 22.728 Kmsg/s, C 83.441 Kmsg/s, C 대비 27.2%, 보류
- 판정:
  - C 기준 갱신으로 65536/131072B 비율은 올랐지만 통과권에는 못 닿았다.
  - 현재 public `TopicMessage` materialize와 JS/native receive 경계를 유지하는 범위에서는
    `subscribePayloadInto` 계열을 되살리지 않고 같은 효과를 내는 후보가 아직 확인되지 않았다.

## materialize single-part fast path 후보

- 대상:
  - `MULTI_DEALER_DEALER`
  - `tcp`
  - `65536,131072B`
- 근거:
  - 이전 log에서 가장 큰 효과가 있었던 `recvInto` 계열은 지금 public surface guard가 금지한다.
  - 같은 효과를 public contract 변경 없이 대체할 수 있는지 확인하기 위해 내부
    `message_materializer`에서 single-part `raw.parts.map(...)`을 빠른 분기로 바꾸는 후보를
    시험했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_141528_node_multi_dd_tcp_large_materialize_parts_probe_20260601.txt`
  - status: complete
- 결과:
  - 65536B: 기존 Node 42.861 Kmsg/s, 후보 34.823 Kmsg/s
  - 131072B: 기존 Node 23.041 Kmsg/s, 후보 22.758 Kmsg/s
- 판정:
  - single-part materialize 분기는 `MULTI_DEALER_DEALER` large 보류 해소에 도움이 되지 않았다.
  - 코드는 되돌렸고, 계획 문서 표에는 반영하지 않는다.

## MULTI_SPOT_SENDSEND raw routing id 재사용 후보

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp`
  - `64,256,1024B`
- 근거:
  - 이전 log의 SPOT 보류 원인이었던 `MsgUnit(B)=4096` 문제는 현재 context
    `autoHwmMsgUnitBytes` 경로로 해결되어 있다.
  - 남은 병목이 매 전송 `RoutingId` wrapping/normalize 비용인지 확인하기 위해
    `sendToSpot` builder와 `recvRouted` send context에서 raw routing id를 재사용하는 후보를 시험했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_141859_node_multi_spot_sendsend_tcp_small_raw_rid_probe_20260601.txt`
  - status: partial
- 결과:
  - 64B: client timeout으로 fail
  - 256B: 후보 41.021 Kops/s
  - 1024B: 후보 39.449 Kops/s
- 판정:
  - partial failure가 발생했고, 성공한 크기도 기존 수치보다 좋아지지 않았다.
  - 코드는 되돌렸고, 계획 문서 표에는 반영하지 않는다.

## single routed tcp small 제한 재측정과 direct sender 후보

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp`
  - `64,256,1024B`
- 근거:
  - 현재 Node 표에서 single routed `tcp` small은 17.4~21.0%로 낮다.
  - 이전 log에는 routed send/recv public builder/envelope 비용이 반복 병목으로 기록되어 있다.
  - 먼저 같은 조건 C/Node를 제한 재측정해 기준선 변동인지 확인했다.
- 측정:
  - Node 재측정: `perf_node_single_linux_20260601_142244_node_single_routed_tcp_small_recheck_20260601.txt`
  - C 재측정: `perf_c_single_linux_20260601_142257_node_single_routed_tcp_small_c_recheck_20260601.txt`
  - Node direct sender 후보: `perf_node_single_linux_20260601_142453_node_single_routed_tcp_small_direct_sender_probe_20260601.txt`
  - status: complete
- 재측정 결과:
  - `DEALER_ROUTER tcp 64B`: Node 258.864 Kmsg/s, C 1438.340 Kmsg/s, C 대비 18.0%, 보류
  - `DEALER_ROUTER tcp 256B`: Node 256.664 Kmsg/s, C 1378.896 Kmsg/s, C 대비 18.6%, 보류
  - `DEALER_ROUTER tcp 1024B`: Node 251.364 Kmsg/s, C 1235.179 Kmsg/s, C 대비 20.4%, 보류
  - `ROUTER_ROUTER tcp 64B`: Node 258.964 Kmsg/s, C 1266.714 Kmsg/s, C 대비 20.4%, 보류
  - `ROUTER_ROUTER tcp 256B`: Node 256.665 Kmsg/s, C 1228.453 Kmsg/s, C 대비 20.9%, 보류
  - `ROUTER_ROUTER tcp 1024B`: Node 248.728 Kmsg/s, C 1155.128 Kmsg/s, C 대비 21.5%, 보류
- direct sender 후보 결과:
  - `DEALER_ROUTER tcp 64B`: 259.170 Kmsg/s, 개선 없음
  - `DEALER_ROUTER tcp 256B`: 160.574 Kmsg/s, 회귀
  - `DEALER_ROUTER tcp 1024B`: 251.783 Kmsg/s, 개선 없음
  - `ROUTER_ROUTER tcp 64B`: 255.045 Kmsg/s, 개선 없음
  - `ROUTER_ROUTER tcp 256B`: 254.931 Kmsg/s, 개선 없음
  - `ROUTER_ROUTER tcp 1024B`: 239.301 Kmsg/s, 회귀
- 판정:
  - C 기준선 변동으로 해소되는 항목이 아니다.
  - sender builder 우회만으로는 개선되지 않았고 일부 크기는 회귀했다.
  - 코드는 되돌렸고, 계획 문서 표에는 반영하지 않는다.

## MULTI_STREAM tcp small raw stream send 후보

- 대상:
  - `MULTI_STREAM`
  - `tcp`
  - `64,256,1024B`
- 근거:
  - 이전 log에서 `MULTI_STREAM` small은 frame 재구성 Buffer와 public stream send builder 비용이
    병목으로 기록되어 있었다.
  - stream runtime 내부에 raw routing id send 경로를 추가하고, perf server echo hot path에서
    `RoutingId` 재정규화와 public builder 생성을 건너뛰는 후보를 시험했다.
  - 공개 contract 문서와 `.d.ts`에는 새 API를 노출하지 않는다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_142752_node_multi_stream_tcp_small_raw_send_probe_20260601.txt`
  - C: `perf_c_multi_linux_20260601_142813_node_multi_stream_tcp_small_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - 64B: Node 94.631 Kops/s, C 331.121 Kops/s, C 대비 28.6%, 보류
  - 256B: Node 92.731 Kops/s, C 308.973 Kops/s, C 대비 30.0%, 통과
  - 1024B: Node 90.571 Kops/s, C 323.117 Kops/s, C 대비 28.0%, 보류
- 판정:
  - 256B는 같은 조건 C 기준으로 통과권에 올랐다.
  - 64B와 1024B는 여전히 보류지만, 1024B는 기존 81.509 Kops/s에서 90.571 Kops/s로
    개선되어 후보를 유지한다.
  - 계획 문서 표에 tcp `MULTI_STREAM` 64/256/1024B 판정을 갱신한다.

## MULTI_STREAM non-tcp small raw stream send 후보 기각

- 대상:
  - `MULTI_STREAM`
  - `ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - tcp에서 효과가 있었던 raw stream send 후보가 framed/tls transport에도 유효한지 확인했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_143309_node_multi_stream_non_tcp_small_raw_send_probe_20260601.txt`
  - status: complete
- 결과:
  - `ws`: 64/256/1024B = 54.467/54.707/51.611 Kops/s
  - `wss`: 64/256/1024B = 53.159/50.353/47.505 Kops/s
  - `tls`: 64/256/1024B = 57.429/55.633/52.554 Kops/s
- 판정:
  - 기존 pending queue 개선 결과보다 낮아졌다.
  - raw send 후보는 tcp에만 적용하고, non-tcp는 기존 public builder 경로를 유지한다.
  - 계획 문서 표에는 반영하지 않는다.

## MULTI_SPOT_SENDSEND small C 기준 제한 재측정

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - 이전 log에서 SPOT 계열의 큰 차이는 `MsgUnit(B)=4096` 조건 불일치와 public/raw
    receive/send 경계 비용으로 나뉘어 있었다.
  - 현재 `MsgUnit(B)` 조건은 context `autoHwmMsgUnitBytes` 경로로 맞춰져 있으므로, 남은
    small 보류가 C 기준선 변동인지 먼저 분리했다.
  - native perf 전용 loop나 `spotPerfSendSendLoop` 복구는 현재 optimization guard가 금지하고
    public contract 변경 성격이 있어 후보에서 제외한다.
- 측정:
  - C: `perf_c_multi_linux_20260601_143553_node_multi_spot_sendsend_small_c_recheck_20260601.txt`
  - Node: `perf_node_multi_linux_20260601_100330_node_multi_spot_sendsend_cstyle_send_sweep_probe_20260601.txt`
  - Node large: `perf_node_multi_linux_20260601_100833_node_multi_spot_sendsend_cstyle_large_probe_20260601.txt`
  - status: complete
- 결과:
  - `tcp`: 64/256/1024B = 19.4/20.9/19.1%, 보류
  - `ws`: 64/256/1024B = 19.6/20.5/21.8%, 보류
  - `wss`: 64/256/1024B = 17.5/19.7/20.7%, 보류
  - `tls`: 64/256/1024B = 19.2/19.9/18.4%, 보류
- 판정:
  - C 기준을 같은 조건으로 갱신해도 small 보류는 해소되지 않는다.
  - 이전에 효과가 컸던 native/raw loop 계열은 현재 guard와 public contract 원칙에 맞지 않아
    되살리지 않는다.
  - 계획 문서 표에는 최신 C 제한 기준 비율만 반영하고, 개선 후보는 더 좁혀서 별도로 찾는다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER tcp small Received 재사용 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp`
  - `64,256,1024B`
- 근거:
  - 이전 log에서 routed echo는 public builder/envelope 비용이 남은 병목으로 기록되어 있었다.
  - 공개 계약을 바꾸지 않는 범위에서 server hot path의 `Received` 객체를 재사용하고,
    pending reply queue를 `shift()` 대신 head-index로 비우는 후보를 시험했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_144452_node_multi_routed_tcp_small_reuse_pending_probe_20260601.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 64/256/1024B`: 185.639/157.018/159.511 Kops/s
  - `MULTI_ROUTER_ROUTER tcp 64/256/1024B`: 131.839/124.545/117.149 Kops/s
- 판정:
  - 기존 즉시 reply 개선 최종값보다 낮아졌고 통과권 보류 해소에 도움이 되지 않았다.
  - 후보 코드는 되돌렸고 계획 문서 표에는 반영하지 않는다.

## single PUBSUB wss 64B payload copy 제거 후보

- 대상:
  - `PUBSUB`
  - `wss`
  - `64B`
- 근거:
  - `PUBSUB wss 64B`는 제한 재측정 기준 34.7%로 기준 바로 아래였다.
  - 기존 single PUBSUB drain은 `Message.data()`를 다시 reusable buffer로 복사한 뒤 stop/header를
    검사했다. 받은 message storage는 다음 receive 전에 즉시 처리하므로 perf hot path에서
    이 복사를 없앨 수 있다.
- 측정:
  - Node: `perf_node_single_linux_20260601_144753_node_single_pubsub_wss64_dataview_probe_20260601.txt`
  - C: `perf_c_single_linux_20260601_140755_node_single_simple64_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - Node 64B: 460.360 Kmsg/s
  - C 64B: 1235.53 Kmsg/s
  - C 대비 37.3%, 통과
- 판정:
  - 기존 428.682 Kmsg/s에서 460.360 Kmsg/s로 올라가며 통과권에 진입했다.
  - public contract 변경 없이 perf hot path의 중복 copy만 줄이는 변경이므로 유지한다.
  - 계획 문서 표에 `PUBSUB wss 64B`를 통과로 반영한다.

## single SPOT tcp large payload copy 제거 후보 기각

- 대상:
  - `SPOT`
  - `tcp`
  - `131072,262144B`
- 근거:
  - `SPOT tcp 131072/262144B`는 각각 28.9/31.0%로 보류다.
  - single SPOT drain도 받은 payload에서 header 크기만 reusable buffer로 복사해 header와 stop
    token을 검사하므로, PUBSUB와 같은 copy 제거 후보를 시험했다.
- 측정:
  - Node: `perf_node_single_linux_20260601_145017_node_single_spot_tcp_large_dataview_probe_20260601.txt`
  - status: complete
- 결과:
  - 131072B: 기존 12.014 Kmsg/s, 후보 10.753 Kmsg/s
  - 262144B: 기존 6.348 Kmsg/s, 후보 5.632 Kmsg/s
- 판정:
  - payload 직접 처리 후보는 large SPOT에서 회귀했다.
  - 코드는 되돌렸고 계획 문서 표에는 반영하지 않는다.

## MULTI_PUBSUB tcp sampled timestamp stamp 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp`
  - `64,256,65536,131072B`
- 근거:
  - client latency는 기본 32개당 1개만 샘플링하므로, server도 샘플 위치의 payload에만
    `currentEpochNs()` timestamp를 쓰고 나머지는 header/seq만 갱신하는 후보를 시험했다.
  - PUBSUB 단일 publisher 순서가 유지되므로 client의 accepted-count 샘플 위치와 server seq를
    맞출 수 있다고 보았다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_145307_node_multi_pubsub_tcp_sampled_stamp_probe_20260601.txt`
  - status: complete
- 결과:
  - 64B: 기존 547.986 Kmsg/s, 후보 534.763 Kmsg/s
  - 256B: 기존 529.894 Kmsg/s, 후보 550.419 Kmsg/s
  - 65536B: 기존 38.061 Kmsg/s, 후보 36.066 Kmsg/s
  - 131072B: 기존 22.728 Kmsg/s, 후보 20.888 Kmsg/s
- 판정:
  - 256B만 개선됐고 나머지 크기는 회귀했으며 통과권에 닿지 않았다.
  - 크기별 안정성이 낮아 코드는 되돌렸고 계획 문서 표에는 반영하지 않는다.

## single routed small raw native recv 후보

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - 이전 log에서 routed small은 public builder/envelope와 JS/native receive 경계가 함께
    병목으로 기록되어 있었다.
  - sender builder 우회 후보는 개선이 없거나 회귀했으므로, 이번에는 public `recv()` 계약은
    그대로 두고 perf drain 내부에서 native raw receive 결과의 첫 payload part만 바로 읽는
    후보를 시험했다.
  - 새 경로는 runtime 내부 private helper와 perf drain에서만 쓰며, `.d.ts` 공개 surface에는
    노출하지 않는다.
- 측정:
  - Node tcp: `perf_node_single_linux_20260601_145635_node_single_routed_tcp_small_raw_recv_probe_20260601.txt`
  - Node non-tcp: `perf_node_single_linux_20260601_145913_node_single_routed_nontcp_small_raw_recv_probe_20260601.txt`
  - C tcp: `perf_c_single_linux_20260601_142257_node_single_routed_tcp_small_c_recheck_20260601.txt`
  - C non-tcp: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `tcp DEALER_ROUTER 64/256/1024B`: C 대비 21.7/22.4/24.3%, 보류
  - `tcp ROUTER_ROUTER 64/256/1024B`: C 대비 24.9/25.6/26.4%, 보류
  - `ws DEALER_ROUTER 64/256/1024B`: C 대비 21.0/23.7/33.1%, 1024B 통과
  - `ws ROUTER_ROUTER 64/256/1024B`: C 대비 24.1/25.5/32.9%, 1024B 통과
  - `wss DEALER_ROUTER 64/256/1024B`: C 대비 20.7/23.5/53.5%, 1024B 통과
  - `wss ROUTER_ROUTER 64/256/1024B`: C 대비 23.5/24.8/53.2%, 1024B 통과
  - `tls DEALER_ROUTER 64/256/1024B`: C 대비 21.4/22.5/34.9%, 1024B 통과
  - `tls ROUTER_ROUTER 64/256/1024B`: C 대비 22.7/23.9/34.9%, 1024B 통과
- 판정:
  - raw receive 후보는 small routed 수치를 전반적으로 올렸고, `ws/wss/tls` 1024B를 통과권에
    올렸다.
  - 64/256B는 여전히 기준에 못 닿지만 public contract 변경 없이 envelope materialize 비용을
    줄이는 실측 개선이므로 유지한다.
  - 계획 문서 표에는 64/256B를 최신 보류 비율로, 1024B 통과 항목은 통과로 반영한다.

## MULTI_DEALER_DEALER tcp large raw native recv 후보 기각

- 대상:
  - `MULTI_DEALER_DEALER`
  - `tcp`
  - `65536,131072B`
- 근거:
  - 이전 log에서 가장 효과가 컸던 `recvInto(buffer, DontWait)` 계열을 public API로 되살리지는
    않는다.
  - 대신 `MessageSocket.recv()` 내부 raw receive를 private helper로 분리하고, perf server가
    public `Received` materialize 없이 payload part만 읽는 후보를 시험했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_150553_node_multi_dd_tcp_large_raw_recv_probe_20260601.txt`
  - status: complete
- 결과:
  - 65536B: 기존 42.861 Kmsg/s, 후보 38.419 Kmsg/s
  - 131072B: 기존 23.041 Kmsg/s, 후보 24.763 Kmsg/s
- 판정:
  - 131072B는 소폭 올랐지만 통과권에 못 닿았고, 65536B는 뚜렷하게 회귀했다.
  - 크기별 안정성이 낮아 후보 코드는 되돌렸고 계획 문서 표에는 반영하지 않는다.

## MULTI_ROUTER_ROUTER tcp small raw echo 후보 기각

- 대상:
  - `MULTI_ROUTER_ROUTER`
  - `tcp`
  - `64,256,1024B`
- 근거:
  - `MULTI_ROUTER_ROUTER tcp` small은 27.7~29.7%로 기준 바로 아래에 남아 있다.
  - 기존 즉시 reply 개선은 queue-before-send 비용을 줄였지만 public `Received` materialize와
    public send builder 경계를 유지한다. 이를 server 내부에서만 raw recv/raw send로 줄이는
    후보를 시험했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_150958_node_multi_rr_tcp_small_raw_echo_probe_20260601.txt`
  - status: complete
- 결과:
  - 64B: 후보 136.783 Kops/s
  - 256B: 후보 132.160 Kops/s
  - 1024B: 후보 125.873 Kops/s
- 판정:
  - 기존 즉시 reply 최종값보다 낮아졌고 통과권을 만들지 못했다.
  - raw recv/raw send를 동시에 쓰는 server echo 후보는 크기별 이득이 없어 되돌렸고,
    계획 문서 표에는 반영하지 않는다.

## single routed tcp large HWM floor 64 후보

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp`
  - `65536,131072,262144B`
- 근거:
  - routed large에는 HWM floor 32가 이미 적용되어 있었지만, tcp는 여전히 17~23%대 보류가
    남아 있었다.
  - non-tcp large는 통과 항목이 많으므로 전체 transport 기본값을 올리지 않고, tcp large에만
    HWM floor 64를 적용하는 후보를 시험했다.
- 측정:
  - env probe: `perf_node_single_linux_20260601_151216_node_single_routed_tcp_large_hwm64_probe_20260601.txt`
  - 기본값 반영 확인: `perf_node_single_linux_20260601_151356_node_single_routed_tcp_large_hwm64_default_final_20260601.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 65536/131072/262144B`: Node 28.834/15.604/8.109 Kmsg/s,
    C 대비 26.3/24.8/24.0%, 보류
  - `ROUTER_ROUTER tcp 65536/131072/262144B`: Node 31.308/16.207/8.208 Kmsg/s,
    C 대비 28.9/26.0/24.3%, 보류
  - Auto-HWM detail에서 tcp routed large sender/receiver가 SNDHWM/RCVHWM 64로 적용됐다.
- 판정:
  - 통과권에는 못 닿았지만 6개 모두 기존 표의 17~23%대보다 개선됐다.
  - public contract 변경 없이 tcp routed large queue 여유만 조정하는 변경이므로 유지한다.
  - non-tcp는 기존 floor 32를 유지해 통과 항목의 정책 변동을 피한다.

## MULTI_PUBSUB small raw subscribe 후보

- 대상:
  - `MULTI_PUBSUB`
  - `tcp,ws,wss,tls`
  - `64,256B`
- 근거:
  - 이전 log에서 `MULTI_PUBSUB`은 `TopicMessage` 재사용만으로 부족했고, 추가 개선은
    raw/typed subscribed receive facade가 필요하다고 정리되어 있었다.
  - public `subscribePayloadInto`류 API는 되살리지 않는다. 대신 runtime 내부에 private
    raw subscribe helper를 두고, perf client hot path에서만 `TopicMessage` materialize를
    건너뛰는 후보를 시험했다.
- 측정:
  - broad tcp probe: `perf_node_multi_linux_20260601_151713_node_multi_pubsub_tcp_raw_subscribe_probe_20260601.txt`
  - all-transport small final: `perf_node_multi_linux_20260601_152140_node_multi_pubsub_small_raw_subscribe_final_20260601.txt`
  - ws 256B public-path recheck: `perf_node_multi_linux_20260601_152256_node_multi_pubsub_ws256_public_path_recheck_20260601.txt`
  - C tcp: `perf_c_multi_linux_20260601_141009_node_multi_pubsub_tcp_low_c_recheck_20260601.txt`
  - C non-tcp: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- broad tcp probe 결과:
  - 64B: 기존 547.986 Kmsg/s, 후보 600.780 Kmsg/s
  - 256B: 기존 529.894 Kmsg/s, 후보 580.089 Kmsg/s
  - 65536B: 기존 38.061 Kmsg/s, 후보 38.108 Kmsg/s
  - 131072B: 기존 22.728 Kmsg/s, 후보 22.087 Kmsg/s
- all-transport small final 결과:
  - `tcp 64/256B`: Node 599.334/593.152 Kmsg/s, C 대비 22.6/23.9%, 보류
  - `ws 64B`: Node 538.310 Kmsg/s, C 대비 23.9%, 보류
  - `ws 256B`: raw 후보 472.098 Kmsg/s로 기존 public path보다 낮아 제외했다.
    public path 재확인값은 509.433 Kmsg/s로 기존과 같은 수준이다.
  - `wss 64/256B`: Node 555.255/592.466 Kmsg/s, C 대비 20.7/21.1%, 보류
  - `tls 64/256B`: Node 561.376/552.992 Kmsg/s, C 대비 22.7/21.2%, 보류
- 판정:
  - raw subscribe는 64/256B 대부분을 개선하지만 통과권까지는 못 올렸다.
  - 131072B broad probe와 `ws 256B`에서 회귀가 확인되어 raw subscribe는 `msgSize <= 256`
    중 `ws 256B`를 제외한 조건에만 적용한다.
  - public `subscribe()` 계약은 유지하고 계획 문서 표에는 개선된 small 비율과 회귀 제외
    조건만 반영한다.

## MULTI_SPOT_SENDSEND small raw routed 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - 이전 log에서 SPOT_SENDSEND는 public send/recv 경계와 routed envelope 비용이 병목 후보로
    남아 있었다.
  - public API를 바꾸지 않고 `Spot` runtime private raw routed receive/send helper를 두고,
    perf client/server에서만 `Received` materialize와 `received.send()` builder를 우회하는
    후보를 시험했다.
- 측정:
  - 실패 probe: `perf_node_multi_linux_20260601_152825_node_multi_spot_sendsend_small_raw_routed_probe_20260601.txt`
  - NoData 처리 보정 뒤 complete probe:
    `perf_node_multi_linux_20260601_153336_node_multi_spot_sendsend_small_raw_routed_probe2_20260601.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 49.902/49.042/48.130 Kops/s
  - `ws 64/256/1024B`: 48.134/42.132/40.143 Kops/s
  - `wss 64/256/1024B`: 43.331/39.067/41.419 Kops/s
  - `tls 64/256/1024B`: 45.043/46.995/40.511 Kops/s
- 판정:
  - 기존 C-style send sweep 최종값보다 낮아졌고 통과권을 만들지 못했다.
  - raw routed receive/send 후보는 되돌렸고 계획 문서 표에는 반영하지 않는다.

## MULTI_STREAM small raw packet handler 후보

- 대상:
  - `MULTI_STREAM`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - `MULTI_STREAM` server는 native packet callback에서 header/body buffer를 `Message`로 감싼 뒤
    다시 `.data()`로 꺼내 echo frame을 만들고 있었다.
  - public `setPacketHandler()` 계약은 유지하고 runtime private raw packet handler를 추가해,
    perf server에서만 native packet buffer로 바로 frame을 만드는 후보를 시험했다.
  - tcp는 기존 raw stream send 후보와 결합하고, non-tcp는 public send 계약을 유지한다.
- 측정:
  - unsafe pending probe:
    `perf_node_multi_linux_20260601_153803_node_multi_stream_small_raw_packet_probe_20260601.txt`
  - tcp pending fix final:
    `perf_node_multi_linux_20260601_153948_node_multi_stream_tcp_small_raw_packet_pendingfix_20260601.txt`
  - non-tcp final:
    `perf_node_multi_linux_20260601_154244_node_multi_stream_nontcp_small_raw_packet_final_20260601.txt`
  - C tcp 기준: `perf_c_multi_linux_20260601_142813_node_multi_stream_tcp_small_c_recheck_20260601.txt`
  - C non-tcp 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 첫 probe는 tcp 256B에서 `malloc(): unaligned tcache chunk detected`가 나와 partial이었다.
    raw routing id buffer를 pending queue에 보관한 것이 callback 생명주기를 벗어날 수 있어,
    pending에는 cached `RoutingId`를 저장하도록 고쳤다.
  - `tcp 64/256/1024B`: Node 110.750/98.548/91.694 Kops/s,
    C 대비 33.4/31.9/28.4%, 64/256B 통과
  - `ws 64/256/1024B`: Node 59.077/57.453/55.720 Kops/s,
    C 대비 20.3/20.8/20.6%, 보류
  - `wss 64/256/1024B`: Node 57.800/55.500/51.714 Kops/s,
    C 대비 29.0/29.2/28.3%, 보류
  - `tls 64/256/1024B`: Node 62.581/61.640/55.562 Kops/s,
    C 대비 26.2/27.5/26.1%, 보류
- 판정:
  - tcp 64B는 보류에서 통과로 올라갔고, tcp 256B는 통과 여유가 커졌다.
  - non-tcp small도 전반적으로 개선됐지만 통과권에는 못 닿았다.
  - public packet handler 계약은 유지하고 private raw packet handler는 perf server hot path에만
    사용하므로 후보를 유지한다.

## single routed tcp large HWM floor 128 후보 기각

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp`
  - `65536,131072,262144B`
- 근거:
  - HWM floor 64는 routed tcp large 6개를 모두 개선했지만 아직 보류가 남아 있다.
  - 같은 정책을 128까지 올리면 추가 queue 여유가 대용량 throughput을 더 올리는지 확인했다.
- 측정:
  - Node: `perf_node_single_linux_20260601_154538_node_single_routed_tcp_large_hwm128_probe_20260601.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 65536/131072/262144B`: 27.127/15.321/8.100 Kmsg/s
  - `ROUTER_ROUTER tcp 65536/131072/262144B`: 32.351/15.215/8.070 Kmsg/s
- 판정:
  - HWM floor 64 최종값인 `DEALER_ROUTER` 28.834/15.604/8.109 Kmsg/s,
    `ROUTER_ROUTER` 31.308/16.207/8.208 Kmsg/s보다 일관되게 좋지 않다.
  - `ROUTER_ROUTER 65536B`만 소폭 올랐지만 나머지 5개가 같거나 낮아져 기본값은 64를 유지한다.
  - 계획 문서 표에는 반영하지 않는다.

## MULTI_PUBSUB selective direct publish 후보

- 대상:
  - `MULTI_PUBSUB`
  - `tcp,ws,wss,tls`
  - `64,256,1024,65536,131072B`
- 근거:
  - server hot path는 매 publish마다 public `publish(topic).message(...).flags(...).submit()`
    builder를 만들고 있었다.
  - public 계약을 바꾸지 않고 runtime `publishDirect()` 내부 경로를 perf server에서만 써서
    builder 생성을 줄이는 후보를 시험했다.
- 측정:
  - broad probe: `perf_node_multi_linux_20260601_155214_node_multi_pubsub_direct_publish_probe_20260601.txt`
  - selective final:
    `perf_node_multi_linux_20260601_155746_node_multi_pubsub_selective_direct_publish_final_20260601.txt`
  - C tcp 기준: `perf_c_multi_linux_20260601_141009_node_multi_pubsub_tcp_low_c_recheck_20260601.txt`
  - C non-tcp 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- broad probe 결과:
  - 기존 sample timestamp 기준 대비 `tcp 64/256/65536`, `tls 64/256/65536`,
    `ws 64/65536`, `wss 64/256/1024/65536/131072`는 개선됐다.
  - `ws 256`, `ws 1024`, `tcp 131072`, `tls 1024/131072` 등은 회귀했다.
  - raw subscribe 최종값과 비교하면 `tcp 64/256`, `wss 256`, `tls 64/256`은 direct publish를
    같이 쓸 때 오히려 낮아져 제외했다.
- selective final 결과:
  - `tcp 65536B`: Node 39.600 Kmsg/s, C 대비 20.6%, 보류
  - `ws 64B`: Node 555.781 Kmsg/s, C 대비 24.7%, 보류
  - `ws 65536B`: Node 36.700 Kmsg/s, C 대비 29.8%, 보류
  - `wss 64B`: Node 576.406 Kmsg/s, C 대비 21.4%, 보류
  - `wss 1024B`: Node 519.931 Kmsg/s, C 대비 32.3%, 통과
  - `wss 65536B`: Node 24.840 Kmsg/s, C 대비 34.5%, 통과
  - `wss 131072B`: Node 13.406 Kmsg/s, C 대비 38.1%, 통과
  - `tls 65536B`: Node 29.721 Kmsg/s, C 대비 30.6%, 통과
- 판정:
  - direct publish는 전체 적용하면 회귀가 섞이므로 transport/size별 allowlist로 제한한다.
  - `tls 65536B`와 `wss 65536B`는 보류에서 통과로 올라갔다.
  - public `publish()` 계약은 유지하고 perf server hot path에서만 내부 direct 경로를 쓴다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER large head-index queue 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,ws`
  - `65536,131072B`
- 근거:
  - 이전 log의 5월 Node routed echo 기록은 public builder/envelope 비용과 함께 pending reply
    queue 비용도 hot path 후보로 남겨 두었다.
  - 공개 계약을 바꾸지 않고 server pending queue의 `Array.shift()`를 head-index queue로 바꾸면
    backlog가 있는 대용량 routed echo에서 배열 이동 비용을 줄일 수 있는지 확인했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_160727_node_multi_routed_large_head_queue_probe_20260601.txt`
  - 비교 기준: `perf_node_multi_linux_20260601_082420_node_multi_routed_immediate_reply_final_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`: 기존 26.871/14.139 Kops/s,
    후보 26.528/14.857 Kops/s
  - `MULTI_DEALER_ROUTER ws 65536/131072B`: 기존 25.053/12.452 Kops/s,
    후보 24.833/12.300 Kops/s
  - `MULTI_ROUTER_ROUTER tcp 65536/131072B`: 기존 26.914/14.503 Kops/s,
    후보 26.360/14.018 Kops/s
  - `MULTI_ROUTER_ROUTER ws 65536/131072B`: 기존 24.354/12.354 Kops/s,
    후보 24.684/12.566 Kops/s
- 판정:
  - 일부 cell은 소폭 개선됐지만 절반 이상이 기존 즉시 reply 최종값보다 낮아졌고 통과권으로
    올라간 항목도 없다.
  - head-index queue 후보는 최종 코드에 남기지 않고 계획 문서 표에도 반영하지 않는다.

## MULTI_SPOT_SENDSEND selective client raw routed receive 후보

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - 이전 raw routed 후보는 client/server 양쪽 receive/send를 함께 우회해 회귀했다.
  - 이번에는 public `recvRouted()` 계약은 유지하고, perf client reply drain에서만 private
    raw routed receive로 `Received` materialize 비용을 분리해 보았다.
- 측정:
  - broad probe: `perf_node_multi_linux_20260601_161522_node_multi_spot_sendsend_client_raw_recv_probe_20260601.txt`
  - selective final:
    `perf_node_multi_linux_20260601_162110_node_multi_spot_sendsend_selective_client_raw_recv_final_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260601_143553_node_multi_spot_sendsend_small_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - broad probe는 `tcp 64/256/1024B`, `ws 256/1024B`, `wss 64/1024B`, `tls 256/1024B`에서
    기존 C-style send sweep보다 높았다.
  - selective final에서는 `tcp 64/256/1024B`가 51.077/50.449/49.621 Kops/s로 재현됐고,
    C 대비 20.9/21.2/22.6%까지 올랐다.
  - `ws 1024B`는 47.264 Kops/s, C 대비 22.2%로 기존 21.8%보다 소폭 올랐다.
  - `wss 64B`는 42.965 Kops/s, C 대비 17.9%로 기존 17.5%보다 소폭 올랐다.
  - `tls`와 `ws 256B`, `wss 1024B`는 broad probe의 개선이 final에서 안정적으로 재현되지 않았다.
- 판정:
  - 통과권까지 올린 항목은 없지만, 회귀가 확인된 조합은 제외하고 재현된 조합만 allowlist로 남긴다.
  - public `recvRouted()` 계약은 그대로 두고, private raw receive는 perf client hot path에서만 쓴다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER client raw reply receive 후보

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,ws`
  - `64,256,1024,65536,131072B`
- 근거:
  - routed multi client는 reply를 public `Received`로 materialize한 뒤 payload만 읽고 있었다.
  - public `recv()` 계약은 유지하고 perf client reply drain에서만 private raw receive로 native
    payload를 바로 읽으면 `Received`/`Message` wrapper 생성 비용을 줄일 수 있는지 확인했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_163300_node_multi_routed_client_raw_recv_probe_20260601.txt`
  - 비교 기준: `perf_node_multi_linux_20260601_082420_node_multi_routed_immediate_reply_final_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 64/256/1024B`: 176.788/164.221/151.878 Kops/s,
    C 대비 39.1/39.0/38.7%, 기존 37.5/36.7/37.0%보다 개선
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`: 24.756/13.201 Kops/s로 기존
    26.871/14.139 Kops/s보다 낮아져 제외
  - `MULTI_DEALER_ROUTER ws 64/256/1024/65536/131072B`: 171.545/160.030/150.507/26.231/12.842 Kops/s,
    C 대비 38.5/37.1/36.4/23.3/30.4%; 131072B는 보류에서 통과로 상승
  - `MULTI_ROUTER_ROUTER tcp 64/256/1024/65536/131072B`: 130.058/124.007/117.091/27.435/14.766 Kops/s,
    C 대비 29.9/29.5/28.6/22.8/28.6%; 개선됐지만 통과권에는 못 닿음
  - `MULTI_ROUTER_ROUTER ws 64B`: 128.279 Kops/s로 기존 128.803 Kops/s보다 낮아져 제외
  - `MULTI_ROUTER_ROUTER ws 256/1024/65536/131072B`: 123.342/116.863/25.152/12.881 Kops/s,
    C 대비 32.1/29.3/24.5/31.1%; 131072B는 보류에서 통과로 상승
- 판정:
  - 회귀 조합은 제외하고, 개선이 확인된 pattern/transport/size만 allowlist로 남긴다.
  - public `recv()` 계약은 그대로 두고, private raw receive는 perf client hot path에서만 쓴다.

## MULTI_PUBSUB small latency sample stride 128 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - 이전 round에서 latency sampling 비용이 PUBSUB hot path에 영향을 주는 것이 확인됐다.
  - 기존 default stride 32보다 더 넓은 stride 128이 작은 메시지에서 timestamp 기록 비용을 더 줄이는지
    확인했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_164113_node_multi_pubsub_small_stride128_probe_20260601.txt`
  - 비교 기준:
    `perf_node_multi_linux_20260601_152140_node_multi_pubsub_small_raw_subscribe_final_20260601.txt`,
    `perf_node_multi_linux_20260601_152256_node_multi_pubsub_ws256_public_path_recheck_20260601.txt`,
    `perf_node_multi_linux_20260601_155746_node_multi_pubsub_selective_direct_publish_final_20260601.txt`
  - status: complete
- 결과:
  - `tcp 256B`는 613.472 Kmsg/s로 기존 raw subscribe 최종값 593.152 Kmsg/s보다 올랐다.
  - `wss 64B`도 588.849 Kmsg/s로 direct publish 최종값 576.406 Kmsg/s보다 올랐다.
  - 반대로 `tcp 64B`, `ws 256B`, `tls 64/256/1024B`는 기존 채택값보다 낮아졌다.
  - 개선된 cell도 C 대비 30% 기준에는 닿지 못했다.
- 판정:
  - 일부 개선은 있지만 transport/size 전반의 회귀가 섞였고 통과로 올라간 항목이 없다.
  - default stride는 바꾸지 않고, 이 후보는 최종 코드에 남기지 않는다.

## MULTI_PUBSUB small throughput HWM profile 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp,ws,wss,tls`
  - `64,256,1024B`
- 근거:
  - PUBSUB small 보류 항목은 sender/receiver 큐 설정의 영향을 크게 받을 수 있다.
  - `PERF_CTX_AUTO_HWM_PROFILE=throughput`으로 작은 메시지 HWM을 더 크게 잡아 drop 없이
    throughput이 올라가는지 확인했다.
- 측정:
  - Node: `perf_node_multi_linux_20260601_164625_node_multi_pubsub_small_throughput_hwm_probe_20260601.txt`
  - 비교 기준:
    `perf_node_multi_linux_20260601_152140_node_multi_pubsub_small_raw_subscribe_final_20260601.txt`,
    `perf_node_multi_linux_20260601_152256_node_multi_pubsub_ws256_public_path_recheck_20260601.txt`,
    `perf_node_multi_linux_20260601_155746_node_multi_pubsub_selective_direct_publish_final_20260601.txt`
  - status: complete
- 결과:
  - `tcp 64B`는 608.369 Kmsg/s로 기존 599.334 Kmsg/s보다 소폭 올랐다.
  - `wss 1024B`는 528.196 Kmsg/s로 direct publish 최종값 519.931 Kmsg/s보다 소폭 올랐다.
  - 반대로 `tcp 256B`, `ws 64/256B`, `tls 64B`는 기존 채택값보다 낮아졌다.
  - 개선된 cell도 C 대비 30% 기준에는 닿지 못했다.
- 판정:
  - HWM profile을 전역 또는 pattern 단위로 바꾸면 이미 개선된 cell을 되돌리는 회귀가 생긴다.
  - transport/size별로 runner 환경을 쪼개는 것은 perf 원칙상 측정 정책을 과하게 복잡하게 만들고,
    통과로 올리는 항목도 없어 최종 코드에 남기지 않는다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER router.reply 단일 part stack fast path

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,ws`
  - `64,256,1024,65536,131072B`
- 근거:
  - Node native `router_reply(...)`는 public `router.reply(...).message(...).submit()` 경로에서
    단일 메시지까지 매번 `std::vector<zlink_msg_t>`를 만들고 있었다.
  - public 계약과 perf runner는 그대로 두고, 단일 part일 때만 stack `zlink_msg_t`를 직접
    `router_reply_parts(...)`에 넘겨 native hot path 할당을 줄였다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_router_reply_single_stack_tcp_ws_final_20260601`
  - Node: `perf_node_multi_linux_20260601_172824_node_multi_router_reply_single_stack_tcp_ws_final_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 38.7/38.4/37.4/23.4/29.1%
  - `MULTI_DEALER_ROUTER ws`: 39.2/36.5/36.9/23.8/31.5%
  - `MULTI_ROUTER_ROUTER tcp`: 31.1/30.9/30.5/24.5/31.6%
  - `MULTI_ROUTER_ROUTER ws`: 32.0/32.5/30.8/25.0/31.4%
- 판정:
  - `MULTI_ROUTER_ROUTER tcp 64/256/1024/131072B`, `MULTI_ROUTER_ROUTER ws 1024/131072B`,
    `MULTI_DEALER_ROUTER ws 131072B`가 통과권으로 올라갔다.
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`, `MULTI_DEALER_ROUTER ws 65536B`,
    `MULTI_ROUTER_ROUTER tcp/ws 65536B`는 여전히 보류다.
  - public contract와 perf 측정 의미를 바꾸지 않는 bindings native 내부 개선이므로 최종 코드와
    계획 문서 표에 반영한다.

## MULTI_SPOT_SENDSEND spot 단일 part stack fast path 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp,ws`
  - `64,256,1024B`
- 근거:
  - SPOT native send/reply/request 경로에도 단일 payload가 `std::vector<zlink_msg_t>`를 거치는
    함수가 남아 있어 `router.reply`와 같은 내부 개선이 가능한지 확인했다.
  - public contract와 perf runner는 바꾸지 않았다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_spot_stack_tcp_ws_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_174142_node_multi_spot_sendsend_spot_stack_tcp_ws_probe_20260601.txt`
  - status: partial
- 결과:
  - `tcp 64B`: 43.183 Kops/s
  - `tcp 256B`: 48.933 Kops/s
  - `tcp 1024B`: client timeout
- 판정:
  - 통과권 개선이 없고 partial failure가 발생했다.
  - SPOT stack fast path 후보는 최종 코드와 계획 문서 표에 반영하지 않고 되돌렸다.

## single routed small RoutingId materialize cache 후보 기각

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp,ws`
  - `64,256,1024B`
- 근거:
  - routed receive materialize는 같은 peer routing id도 매 수신마다 `RoutingId.from(Buffer)`로
    새 객체와 복사본을 만든다.
  - public `RoutingId` 값 의미는 유지하면서 runtime 내부 cache로 반복 wrapping 비용을 줄일 수
    있는지 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_rid_cache_tcp_ws_probe_20260601`
  - Node: `perf_node_single_linux_20260601_174846_node_single_routed_rid_cache_tcp_ws_probe_20260601.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 18.1/18.3/21.2%
  - `DEALER_ROUTER ws`: 18.9/20.7/28.8%
  - `ROUTER_ROUTER tcp`: 21.4/21.3/22.6%
  - `ROUTER_ROUTER ws`: 21.7/22.6/28.4%
- 판정:
  - 일부 cell은 기존 표보다 소폭 올랐지만 통과권으로 올라간 항목이 없다.
  - 전역 cache 복잡도를 추가할 만큼 확실한 개선이 아니므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER native receive 단일 part fast path 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,ws`
  - `64,256,1024,65536,131072B`
- 근거:
  - native `socket_recv_message`/`router_recv_message`는 단일 part 메시지도
    `std::vector<zlink_msg_t>`에 move한 뒤 JS snapshot을 만든다.
  - public receive 계약과 perf runner 의미를 유지하면서, 단일 part에서는 vector 할당을
    건너뛰는 후보를 시험했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_single_recv_part_fastpath_tcp_ws_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_180410_node_multi_single_recv_part_fastpath_tcp_ws_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 40.4/39.0/38.7/23.6/30.2%
  - `MULTI_DEALER_ROUTER ws`: 39.8/37.1/36.2/23.2/31.0%
  - `MULTI_ROUTER_ROUTER tcp`: 31.9/29.9/29.6/23.7/31.1%
  - `MULTI_ROUTER_ROUTER ws`: 32.7/32.0/30.0/24.4/32.0%
- 판정:
  - `MULTI_DEALER_ROUTER tcp 131072B`는 보류권에서 통과권으로 올라갔지만,
    `MULTI_ROUTER_ROUTER tcp 256/1024B`는 기존 `router.reply` stack fast path 최종값보다
    낮아져 통과권을 잃을 수 있다.
  - 전수 기준으로 보류 해소보다 회귀 위험이 커서 최종 코드와 계획 문서 표에는 반영하지
    않고 되돌렸다.

## Runtime operation builder payload 상태 흡수 후보 기각

- 대상:
  - `MULTI_PUBSUB`, `MULTI_SPOT_SENDSEND`
  - `tcp,ws`
  - 64/256/1024B
- 근거:
  - `send().message(...).flags(...).submit()` 계열은 operation 객체마다
    `OperationPayload` 객체를 추가로 만든다.
  - public operation builder 계약은 유지하고 runtime builder 내부 필드로 단일/multipart
    상태를 흡수해 작은 메시지 송신 할당을 줄일 수 있는지 확인했다.
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_PUBSUB,MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_runtime_payload_inline_tcp_ws_small_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_181605_node_multi_runtime_payload_inline_tcp_ws_small_probe_20260601.txt`
  - status: complete
- 결과 1:
  - `MULTI_PUBSUB tcp`: 541.282/529.421/499.352 Kmsg/s
  - `MULTI_PUBSUB ws`: 519.773/489.866/516.837 Kmsg/s
  - `MULTI_SPOT_SENDSEND tcp`: 52.165/51.145/49.707 Kops/s
  - `MULTI_SPOT_SENDSEND ws`: 49.445/49.328/48.545 Kops/s
- 판정 1:
  - `MULTI_SPOT_SENDSEND`는 전반적으로 올랐지만 `MULTI_PUBSUB ws 256B`가 기존 채택값보다
    크게 낮아졌다.
  - publish 경로까지 전역 적용하는 형태는 기각했다.
- 측정 2:
  - publish 경로는 기존 `OperationPayload`로 되돌리고, send/request/reply runtime builder만
    내부 필드 상태로 바꾼 뒤 재측정했다.
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_runtime_send_payload_inline_spot_sendsend_tcp_ws_small_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_181922_node_multi_runtime_send_payload_inline_spot_sendsend_tcp_ws_small_probe_20260601.txt`
  - status: complete
- 결과 2:
  - `MULTI_SPOT_SENDSEND tcp`: 50.651/50.728/48.964 Kops/s
  - `MULTI_SPOT_SENDSEND ws`: 46.195/46.729/42.424 Kops/s
- 판정 2:
  - tcp는 기존보다 올랐지만 ws 64/1024B가 기존 채택값보다 낮아졌다.
  - transport별로 runtime builder 구현을 갈라 적용할 수 없고, 통과권으로 올라간 항목도 없어
    최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## Node full verification

- 단일 suite:
  - 명령: `PERF_FULL_MATRIX=1 bindings/node/perf/run_benchmarks.sh --results-tag node_final_current_single_full_20260601`
  - Node: `perf_node_single_linux_20260601_184729_node_final_current_single_full_20260601.txt`
  - status: complete
  - expected/actual: 1020/1020
- multi suite:
  - 명령: `PERF_FULL_MATRIX=1 bindings/node/perf/run_benchmarks_multi.sh --results-tag node_final_current_multi_full_20260601`
  - Node: `perf_node_multi_linux_20260601_193339_node_final_current_multi_full_20260601.txt`
  - status: partial
  - expected/actual: 920/915
  - failure: `MULTI_SPOT_REQREP current tls 1024B`
- multi suite 실패 행 재확인:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tls --pattern MULTI_SPOT_REQREP --msg-sizes 1024 --duration 5 --runs 1 --results-tag node_multi_spot_reqrep_tls_1024_recheck_20260601`
  - Node: `perf_node_multi_linux_20260601_193521_node_multi_spot_reqrep_tls_1024_recheck_20260601.txt`
  - status: complete
  - expected/actual: 5/5
  - 결과: 98.980 Kops/s, 202.712 MB/s, 평균 지연 0.443 ms
- 판정:
  - 채택된 `router.reply` 단일 part stack fast path 자체는 full multi의 routed 계열에서 결과를
    냈다.
  - multi full은 SPOT req/rep tls 1024B 한 행 실패로 partial이었지만, 같은 행의 targeted
    재확인은 complete다. 현재 코드의 고정 실패 증거는 없고, Node 단계의 남은 미달 성능
    개선은 기존 보류/기각 후보 기록을 기준으로 계속 판단한다.

## 10% 게이트 재개 후 Node routed recv 내부 비용 축소 후보

- 재개 기준:
  - 언어별 미달 비율을 다시 계산했다.
  - Node single 33/144, Node multi 52/156으로 둘 다 10%를 넘으므로 다음 언어로
    넘어가지 않고 Node에서 반복 개선을 계속한다.
- 채택 후보:
  - native가 만든 routing id `Buffer`는 이미 Node 소유 값이므로, 내부 materializer가
    다시 복사하지 않고 `RoutingId.fromOwnedBuffer()`로 감싸도록 했다.
  - 일반 routed recv는 `requestSeq`가 없는데도 매번 reply closure를 만들고 있었다.
    request 메시지일 때만 reply closure를 만들도록 줄였다.
  - `raw.parts.map(...)` materializer를 단일 part fast path와 for-loop로 바꿔 callback
    할당 비용을 줄였다.
  - 모두 public contract를 바꾸지 않는 내부 변경이다. `dist/index.d.ts` diff는 비어 있다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- single routed 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_materialize_parts_fastpath_probe_20260601`
  - Node: `perf_node_single_linux_20260601_212351_node_single_routed_materialize_parts_fastpath_probe_20260601.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
  - 결과: 2/24 통과, 평균 25.3%. 기존 accepted routed small 계열보다 통과 수는 늘지 않았다.
  - 판정: single routed small은 여전히 미달이 대부분이므로 Node 10% 게이트는 해소되지 않았다.
- multi routed 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_routingid_owned_buffer_routed_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_210907_node_multi_routingid_owned_buffer_routed_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
  - 결과: routed small은 전반적으로 통과권이다. `MULTI_DEALER_ROUTER tcp 131072B`는 31.9%로
    통과권에 올랐다. `tcp/ws 65536B`는 여전히 23.5~25.6%대로 미달이다.
  - 판정: multi routed에는 일부 미달 해소가 있어 내부 변경은 유지한다. 다만 Node 전체
    미달률은 아직 10% 초과라 다음 언어로 넘어갈 수 없다.

## RuntimeSendOperation 단일 메시지 fast path 후보 기각

- 근거:
  - `send().message(...).flags(...).submit()` 경로의 `OperationPayload` 객체 생성을 줄이는
    후보를 다시 `RuntimeSendOperation`에 한정해 적용했다.
  - public builder contract는 유지했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_send_builder_fastpath_probe_20260601`
  - Node: `perf_node_single_linux_20260601_211858_node_single_routed_send_builder_fastpath_probe_20260601.txt`
  - status: complete
- 판정:
  - 통과 수가 늘지 않았고 일부 transport 값은 기존 후보보다 낮았다.
  - 성능 근거가 부족하므로 최종 코드에 남기지 않고 되돌렸다.

## Received send/reply 단일 메시지 submit 후보 기각

- 근거:
  - `MULTI_SPOT_SENDSEND` 서버 echo는 `received.send().message(part).flags(...).submit()`을
    반복한다.
  - 단일 part에서도 `ReceivedSendOperation`이 `consumeParts()`로 배열을 만들기 때문에,
    public builder API는 유지하고 내부 submit 값만 단일 `Message`로 넘기는 후보를 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,131072 --duration 1 --runs 3 --results-tag node_multi_received_send_single_payload_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_215213_node_multi_received_send_single_payload_probe_20260601.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current ws 64B` client timeout
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,131072 --duration 1 --runs 3 --results-tag node_multi_received_send_single_payload_tcp_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_215355_node_multi_received_send_single_payload_tcp_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `MULTI_SPOT_SENDSEND tcp`: 19.9/19.5/21.4/27.5%
- 판정:
  - complete 재측정에서 통과 항목이 늘지 않았다.
  - 넓은 측정도 partial이어서 채택 근거로 사용할 수 없다.
  - 최종 코드에 남기지 않고 되돌렸다.

## SPOT routed recv requestSeq=0 reply context 제거 후보 채택

- 근거:
  - `MULTI_SPOT_SENDSEND`의 일반 send echo는 `requestSeq=0n`으로 들어온다.
  - `Received.reply()`는 기존에도 `0n` requestSeq를 reply 불가로 처리한다.
  - 그런데 materializer는 `requestSeq !== null`만 보고 reply context를 만들어 일반 send
    수신마다 불필요한 reply closure를 생성했다.
  - `requestSeq=0n`은 기존 의미 그대로 reply 불가로 두고, reply context 생성만 건너뛰도록
    내부 조건을 좁혔다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_no_replyctx_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_220203_node_multi_spot_sendsend_no_replyctx_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.0/19.3/20.8/26.2%
  - `ws`: 16.2/18.4/21.7/36.0%
  - `wss`: 17.5/19.1/21.2/63.2%
  - `tls`: 17.8/20.7/18.9/53.6%
- 판정:
  - `MULTI_SPOT_SENDSEND ws 131072B`는 기존 34.5% 미달권에서 36.0% 통과권으로 올라갔다.
  - small은 여전히 전부 미달이라 Node 10% 게이트는 해소되지 않았다.
  - public contract 변경 없이 의미 없는 reply context만 제거한 내부 개선이므로 유지한다.

## native message property stub 조회 제거 후보 채택

- 대상:
  - `MULTI_PUBSUB`
  - 공통 native receive snapshot 생성 경로
- 근거:
  - 현재 core의 `zlink_msg_gets()`는 모든 property 조회에 `NULL`을 반환하는 스텁이다.
  - Node native snapshot 생성은 data-only receive에서도 `Socket-Type`, `User-Id`,
    `Peer-Address`, `Routing-Id`, `Identity`를 매 메시지마다 조회하고 있었다.
  - public `Message.getProperty()` 의미는 유지한다. 현재 core가 실제 property를 제공하지
    않으므로 non-routed 메시지는 기존과 같이 `null`이고, Node가 기존에 합성하던 routed
    `Routing-Id`/`Identity` property만 그대로 유지한다.
- 검증:
  - `npm run build`: 통과
  - `npm run rebuild-native`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_no_msg_gets_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_221603_node_multi_pubsub_no_msg_gets_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 1:
  - `tcp`: 19.6/22.3/49.3/17.9/25.7%
  - `ws`: 23.1/21.4/26.3/33.4/35.3%
  - `wss`: 19.6/19.6/32.5/36.0/38.2%
  - `tls`: 21.4/20.2/28.0/30.1/32.7%
- 판정 1:
  - `MULTI_PUBSUB ws 65536B`, `ws 131072B`, `wss 65536B`, `tls 65536B`가 통과권으로
    올라갔다.
  - complete 측정에서 통과 항목이 늘었고 public surface 변경이 없으므로 후보를 유지한다.
  - 계획 문서의 `MULTI_PUBSUB` 행은 이 complete 결과로 갱신한다.
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_no_msg_gets_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_221921_node_multi_spot_sendsend_no_msg_gets_probe_20260601.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current tcp 1024B` client timeout
- 판정 2:
  - partial 결과는 표 반영 근거로 쓰지 않는다.
  - 이미 `MULTI_PUBSUB` complete 결과에서 후보 채택 근거가 있으므로 코드는 유지하되,
    `MULTI_SPOT_SENDSEND` 표는 이 partial 결과로 갱신하지 않는다.

## native data-only snapshot Buffer 직접 반환 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - property/refCount가 없는 data-only receive snapshot 경로
- 근거:
  - native가 `{ data: Buffer }` snapshot 객체를 만든 뒤 TypeScript materializer가 다시
    `Message`로 감싸는 비용을 줄이기 위해, data-only snapshot은 내부적으로 `Buffer`를
    바로 넘기는 후보를 시험했다.
  - public `Message`/`TopicMessage` contract와 `.d.ts` surface는 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
  - `npm run rebuild-native`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_direct_buffer_snapshot_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_222859_node_multi_pubsub_direct_buffer_snapshot_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.3/22.9/50.9/18.0/25.3%
  - `ws`: 23.9/23.4/27.6/32.6/34.8%
  - `wss`: 21.4/20.3/33.7/34.8/36.0%
  - `tls`: 21.9/20.8/29.6/31.5/33.1%
- 판정:
  - 기존 채택값 대비 small은 대체로 올랐지만 통과 항목 수가 늘지 않았다.
  - `ws 65536B`, `ws 131072B`, `wss 65536B`, `wss 131072B`는 기존 채택값보다 낮아졌다.
  - goal 기준에 따라 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_STREAM peer-index routing id cache 후보 기각

- 대상:
  - `MULTI_STREAM`
  - stream packet handler의 routing id 전달 경로
- 근거:
  - stream packet handler native callback은 매 packet마다 routing id Buffer를 복사하고,
    TypeScript runtime은 그 Buffer를 latin1 key로 바꿔 `RoutingId` cache를 조회한다.
  - public `setPacketHandler(handler(sourceRid, header, body))` 계약은 유지하고, 내부 native
    callback payload만 peer index와 최초 routing id로 바꿔 반복 routing id 복사와 key 생성을
    줄이는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `npm run rebuild-native`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag node_multi_stream_peer_index_rid_cache_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_223352_node_multi_stream_peer_index_rid_cache_probe_20260601.txt`
  - status: partial
  - 실패: `MULTI_STREAM current tcp 64B` client failed, server 측 `double free or corruption (fasttop)` 발생
- 판정:
  - native callback lifetime에 안전하지 않은 변경이다.
  - partial 결과는 표 반영 근거로 사용할 수 없고, 안전성 문제가 있으므로 최종 코드와 계획
    문서 표에는 반영하지 않고 되돌렸다.

## Message send normalization payload Buffer 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `Received.send().message(part)`처럼 수신 `Message`를 다시 전송하는 내부 payload 정규화 경로
- 근거:
  - 기존 `normalizeMessageLikePayload()`/`normalizeOperationPayload()`는 `Message` 입력을
    `{ data, refCount, properties, metadata }` snapshot 객체로 바꾼 뒤 native send에 넘긴다.
  - native send 초기화는 현재 snapshot의 `data`만 사용하므로, public contract를 바꾸지 않고
    내부적으로 `Message.payloadBuffer()`를 바로 넘겨 객체 생성과 native property lookup을 줄이는
    후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `npm run rebuild-native`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 전체 large/small probe:
    `perf_node_multi_linux_20260601_224755_node_multi_spot_sendsend_message_payload_buffer_probe_20260601.txt`
    (`status=partial`, `tls 65536B` timeout)
  - tls large recheck:
    `perf_node_multi_linux_20260601_224905_node_multi_spot_sendsend_message_payload_buffer_tls_large_recheck_20260601.txt`
    (`status=complete`)
  - tcp/ws/wss large 묶음:
    `perf_node_multi_linux_20260601_225318_node_multi_spot_sendsend_message_payload_buffer_large_complete_20260601.txt`
    (`status=partial`, `wss 131072B` timeout)
  - wss 131072B recheck:
    `perf_node_multi_linux_20260601_225358_node_multi_spot_sendsend_message_payload_buffer_wss_131072_recheck_20260601.txt`
    (`status=complete`)
  - tcp/ws/wss 65536B recheck:
    `perf_node_multi_linux_20260601_225536_node_multi_spot_sendsend_message_payload_buffer_65536_complete_20260601.txt`
    (`status=complete`)
  - tcp/ws 131072B recheck:
    `perf_node_multi_linux_20260601_225640_node_multi_spot_sendsend_message_payload_buffer_tcp_ws_131072_complete_20260601.txt`
    (`status=complete`)
- 결과:
  - complete recheck에서 large payload bandwidth는 일부 개선됐다.
    - `tcp 65536B`: 2215.117 -> 2535.457 MB/s
    - `tcp 131072B`: 1796.211 -> 1889.534 MB/s
    - `ws 65536B`: 2145.911 -> 2261.778 MB/s
    - `ws 131072B`: 2281.701 -> 2527.855 MB/s
    - `wss 65536B`: 1213.596 -> 1338.507 MB/s
    - `wss 131072B`: 1429.209 -> 1493.172 MB/s
    - `tls 65536B`: 1499.726 -> 1648.230 MB/s
    - `tls 131072B`: 1671.168 -> 1713.897 MB/s
- 판정:
  - 개선폭은 있으나 새로 `미달`에서 `통과`로 바뀐 셀이 없다. `tcp 65536B`와 `tcp 131072B`는
    여전히 목표 미달이고, 나머지 large 셀은 기존에도 통과 상태였다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - partial probe가 두 번 발생했으므로 표 반영 근거도 제한적이다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## Received._replace no-op close loop 제거 후보 채택

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - single routed receive hot path
- 근거:
  - caller-provided `Received`를 재사용할 때 `_replace()`는 이전 `parts`를 순회하며
    `Message.close()`를 호출한다.
  - 현재 `Message.close()`는 no-op이고 수신 payload는 JS `Buffer` 소유라 이 순회는
    관측 가능한 해제 동작 없이 per-message 비용만 만든다.
  - public contract와 `.d.ts` surface는 바꾸지 않고, reusable receive refill 내부의 no-op
    close loop만 제거했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_received_replace_no_close_probe_20260601`
  - Node: `perf_node_single_linux_20260601_230232_node_single_routed_received_replace_no_close_probe_20260601.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 18.9/19.3/22.0%
  - `DEALER_ROUTER ws`: 19.7/21.8/30.5%
  - `DEALER_ROUTER wss`: 19.2/21.2/49.1%
  - `DEALER_ROUTER tls`: 20.0/20.7/32.6%
  - `ROUTER_ROUTER tcp`: 22.0/22.6/23.1%
  - `ROUTER_ROUTER ws`: 22.6/23.5/30.6%
  - `ROUTER_ROUTER wss`: 22.2/22.5/50.0%
  - `ROUTER_ROUTER tls`: 21.4/21.8/31.9%
- 판정:
  - `DEALER_ROUTER ws 1024B`, `DEALER_ROUTER tls 1024B`,
    `ROUTER_ROUTER ws 1024B`, `ROUTER_ROUTER tls 1024B`가 통과권으로 올라갔다.
  - complete 측정에서 통과 항목이 늘었고 public surface 변경이 없으므로 후보를 유지한다.
  - 계획 문서의 Node single routed small 행은 이 complete 결과로 갱신한다.
- multi routed large 추가 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,MULTI_DEALER_DEALER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_large_after_received_replace_no_close_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_233204_node_multi_routed_large_after_received_replace_no_close_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- multi 결과:
  - `MULTI_DEALER_DEALER tcp`: 36.9/38.5%
  - `MULTI_DEALER_DEALER ws`: 41.5/43.6%
  - `MULTI_DEALER_ROUTER tcp`: 26.0/32.4%
  - `MULTI_DEALER_ROUTER ws`: 25.8/34.1%
  - `MULTI_ROUTER_ROUTER tcp`: 27.1/33.4%
  - `MULTI_ROUTER_ROUTER ws`: 28.0/35.0%
- multi 판정:
  - `MULTI_DEALER_DEALER tcp 65536B`, `MULTI_DEALER_DEALER tcp 131072B`,
    `MULTI_DEALER_ROUTER tcp 131072B`가 통과권으로 올라갔다.
  - `MULTI_DEALER_ROUTER tcp/ws 65536B`, `MULTI_ROUTER_ROUTER tcp/ws 65536B`는 개선됐지만
    아직 미달이다.
  - 계획 문서의 Node multi routed large 행은 이 complete 결과로 갱신한다.

## MULTI_SPOT_SENDSEND raw routing id send-context 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `Spot.recvRouted(...).send()` 내부 send context
- 근거:
  - `recvRouted()` raw 결과에는 `sourceRid`/`spotRid` Buffer가 이미 들어 있다.
  - 기존 send context는 `received.send().submit()` 시마다 이 Buffer를 `RoutingId.from()`으로
    복사한 뒤 `sendToSpotDirect()`에서 다시 `normalizeRoutingId()`로 Buffer를 꺼낸다.
  - public `Received.send()` 계약은 유지하고, 내부 send context에서 raw Buffer를 바로
    `spotSendToSpot`으로 넘기는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - small 전체 probe:
    `perf_node_multi_linux_20260601_231125_node_multi_spot_sendsend_raw_rid_send_context_probe_20260601.txt`
    (`status=partial`, `ws 1024B` timeout)
  - ws 1024B recheck:
    `perf_node_multi_linux_20260601_231203_node_multi_spot_sendsend_raw_rid_send_context_ws1024_recheck_20260601.txt`
    (`status=complete`)
- 결과:
  - partial probe와 complete recheck를 함께 보면 `tcp` small은 21.0/20.3/21.0%,
    `ws 1024B`는 22.4%다.
- 판정:
  - `MULTI_SPOT_SENDSEND` small에서 새로 통과권에 오른 항목이 없다.
  - partial probe는 표 반영 근거로 사용할 수 없고, complete recheck도 미달이다.
  - goal 기준상 통과 항목이 늘지 않은 후보이므로 최종 코드와 계획 문서 표에는 반영하지
    않고 되돌렸다.

## TopicMessage._replace no-op close loop 제거 후보 기각

> 2026-06-02 보정: 이 절의 multi PUBSUB 측정만으로는 새 통과 항목이 없어 당시에는
> 되돌렸지만, 이후 single `PUBSUB wss 64B` targeted 측정에서 통과 항목을 만들어
> 같은 내부 변경을 채택했다. multi PUBSUB 행도 이 complete 측정 파일의 current-code
> 값으로 정리했다.

- 대상:
  - `MULTI_PUBSUB`
  - reusable `TopicMessage` subscribe hot path
- 근거:
  - `TopicMessage._replace()`도 `Received._replace()`와 같이 이전 parts를 순회하며
    `Message.close()`를 호출한다.
  - 현재 `Message.close()`는 no-op이므로, public contract를 바꾸지 않고 이 순회를 제거하는
    후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_topic_replace_no_close_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_232020_node_multi_pubsub_topic_replace_no_close_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.2/22.4/51.6/18.4/26.9%
  - `ws`: 24.5/22.5/27.4/36.0/37.0%
  - `wss`: 20.9/19.9/32.9/37.2/38.8%
  - `tls`: 22.3/21.0/28.8/33.1/36.0%
- 판정:
  - 기존 채택값 대비 일부 수치는 올랐지만 새로 통과권에 오른 셀이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_STREAM Message.fromOwnedBuffer 후보 기각

- 대상:
  - `MULTI_STREAM`
  - stream packet handler의 header/body `Message` wrapping 경로
- 근거:
  - `messageFromNativeBuffer()`는 stream packet callback의 header/body Buffer를
    `Message.fromSnapshot({ data })`로 감싸며, packet마다 snapshot 객체를 만든다.
  - public `setPacketHandler()` 계약은 유지하고 내부 `Message.fromOwnedBuffer()` helper로
    snapshot 객체 생성을 줄이는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_message_from_owned_buffer_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_232624_node_multi_stream_message_from_owned_buffer_probe_20260601.txt`
  - C 기준: tcp는 `perf_c_multi_linux_20260601_142813_node_multi_stream_tcp_small_c_recheck_20260601.txt`, non-tcp는 `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 30.4/29.4/25.9%
  - `ws`: 19.5/20.0/19.6%
  - `wss`: 28.5/28.4/27.0%
  - `tls`: 25.9/26.7/25.7%
- 판정:
  - 기존 accepted stream 결과보다 모든 측정 셀이 낮다.
  - 특히 `tcp 256B`는 기존 통과권에서 29.4% 미달권으로 내려갈 수 있다.
  - 회귀 후보이므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_STREAM packet routing id fromOwnedBuffer 후보 기각

- 대상:
  - `MULTI_STREAM`
  - packet handler routing id cache의 최초 wrapping 경로
- 근거:
  - `StreamSocket.packetRoutingId()`는 native callback에서 받은 routing id Buffer를 latin1 key로
    cache한 뒤 `wrapRoutingId()`로 복사해 `RoutingId`를 만든다.
  - native callback payload 구조를 바꾸지 않고, 이미 존재하는 내부 `RoutingId.fromOwnedBuffer()`로
    최초 wrapping 복사를 줄이는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_packet_rid_owned_buffer_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_233844_node_multi_stream_packet_rid_owned_buffer_probe_20260601.txt`
  - status: partial
  - 실패: `MULTI_STREAM current tcp 64B` client failed, server 측 `malloc(): unaligned tcache chunk detected` 발생
- 판정:
  - native callback에서 받은 routing id Buffer를 cache에 보관하는 것은 callback lifetime을 벗어나
    안전하지 않다.
  - partial 결과는 표 반영 근거로 사용할 수 없고, 안전성 문제가 있으므로 최종 코드와 계획
    문서 표에는 반영하지 않고 되돌렸다.

## routed receive raw 객체 shape 축소 후보 기각

- 대상:
  - `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
- 근거:
  - native `create_recv_message_value()`는 JS materializer가 읽지 않는 `hasMore=false`를
    매 수신 객체에 설정한다.
  - `create_router_recv_message_value()`는 일반 one-way routed 수신에서 `requestSeq=null`을
    설정하지만, JS 경로는 속성이 없어도 `raw.requestSeq ?? null`로 같은 public 값을 만든다.
  - public `.d.ts`와 공개 contract를 바꾸지 않고 native 내부 raw 객체의 속성 생성을 줄이는
    후보를 시험했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024,4096,65536,131072 --duration 1 --runs 3 --results-tag node_single_routed_raw_shape_probe_20260601`
  - Node: `perf_node_single_linux_20260601_234934_node_single_routed_raw_shape_probe_20260601.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과 1:
  - `DEALER_ROUTER tcp`: 20.2/20.1/23.5/23.4/25.7%
  - `DEALER_ROUTER ws`: 20.5/22.8/31.7/47.0/43.7%
  - `DEALER_ROUTER wss`: 20.6/22.6/51.9/96.6/107.2%
  - `DEALER_ROUTER tls`: 21.0/21.6/34.3/78.9/85.0%
  - `ROUTER_ROUTER tcp`: 23.4/23.8/24.9/24.1/25.3%
  - `ROUTER_ROUTER ws`: 23.9/24.9/32.2/46.3/44.4%
  - `ROUTER_ROUTER wss`: 23.5/24.2/52.9/98.2/107.4%
  - `ROUTER_ROUTER tls`: 22.9/23.4/34.3/77.9/85.2%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536 --duration 1 --runs 3 --results-tag node_multi_routed_65536_raw_shape_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_235230_node_multi_routed_65536_raw_shape_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `MULTI_DEALER_ROUTER tcp/ws 65536B`: 25.7/25.5%
  - `MULTI_ROUTER_ROUTER tcp/ws 65536B`: 26.8/28.1%
- 판정:
  - single routed는 여러 셀이 올랐지만 새로 미달에서 통과로 바뀐 셀이 없다.
  - multi routed 65536B는 새 통과가 없고, 기존 accepted 값 대비 `MULTI_DEALER_ROUTER`
    tcp/ws와 `MULTI_ROUTER_ROUTER tcp`가 낮아졌다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀가 있는 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## materializeParts native parts 배열 재사용 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - receive materializer의 `Message[]` 구성 경로
- 근거:
  - native raw 객체는 이미 새 `parts` 배열을 만들어 JS로 넘긴다.
  - public `Received.parts`/`TopicMessage.parts`는 frozen `Message[]`만 보장하면 되므로,
    materializer가 별도 `Message[]`를 만들지 않고 native raw 배열을 내부 소유 배열로
    전환할 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_pubsub_materialize_parts_inplace_probe_20260601`
  - Node: `perf_node_multi_linux_20260601_235832_node_multi_pubsub_materialize_parts_inplace_probe_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 19.7/22.5/50.2%
  - `ws`: 23.9/22.0/26.4%
  - `wss`: 20.9/20.5/33.2%
  - `tls`: 21.4/20.6/28.7%
- 판정:
  - 기존 accepted PUBSUB 값보다 일부는 소폭 올랐지만 새로 미달에서 통과로 바뀐 셀이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT recvRouted reply closure 지연 생성 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `Spot.recvRouted()`의 materialize callback 구성 경로
- 근거:
  - 일반 send-send echo는 `requestSeq=0`으로 들어오며 reply 불가다.
  - materializer는 이미 `requestSeq=0`에서 reply context를 만들지 않지만,
    `Spot.recvRouted()` 호출부는 매 수신마다 reply closure를 만들어 넘긴다.
  - public `Received.reply()`/`Received.send()` 의미는 그대로 두고, reply 가능한 request에서만
    reply closure를 만들도록 줄이는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_lazy_reply_closure_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_000920_node_multi_spot_sendsend_lazy_reply_closure_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current tls 256B` client timeout
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_lazy_reply_closure_tcp_ws_recheck_20260602`
  - Node: `perf_node_multi_linux_20260602_001308_node_multi_spot_sendsend_lazy_reply_closure_tcp_ws_recheck_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `tcp`: 20.6/20.1/19.3/29.4/31.0%
  - `ws`: 19.7/20.5/22.7/42.5/36.7%
- 판정:
  - `tcp 131072B`는 기존 26.0%에서 31.0%로 올랐지만 SPOT 기준 33%를 넘지 못했다.
  - `ws 65536B/131072B`는 기존에도 통과였고 새 통과 항목은 없다.
  - 넓은 측정은 partial이라 표 근거로 사용할 수 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT routed raw alias 제거 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - native `create_spot_routed_value()`의 raw 수신 객체 생성 경로
- 근거:
  - TypeScript `SpotRoutedRaw`와 `Spot.recvRouted()`는 `sourceRid`와 `spotRid`를 사용한다.
  - native raw 객체가 같은 routing id를 `sourceNodeRid`/`sourceSpotRid` 별칭으로도 설정하고 있어
    send-send hot path에서 불필요한 property 설정을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_routed_alias_drop_tcp_ws_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_001831_node_multi_spot_sendsend_routed_alias_drop_tcp_ws_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 18.1/19.9/21.8/30.4/27.1%
  - `ws`: 17.9/19.1/22.8/43.0/38.3%
- 판정:
  - `ws 65536B/131072B`는 기존에도 통과권이었고 새 통과 항목은 없다.
  - `tcp 65536B/131072B`는 개선 후보 적용 뒤에도 SPOT 기준 33%에 못 닿아 미달이다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## caller-provided storage parts 배열 동결 제거 후보 기각

- 대상:
  - `Received._replace()`와 `TopicMessage._replace()`의 caller-provided storage 재사용 경로
  - single routed small, multi `MULTI_PUBSUB` small
- 근거:
  - `_replace()`는 native에서 받은 새 `Message[]`를 곧바로 보관하는 경로지만 매 수신마다
    `Object.freeze(parts)`를 호출한다.
  - d.ts와 공통 spec의 public shape는 `parts: Message[]`/`List<Message>`이고 별도 불변
    배열 계약은 없으므로, runtime의 추가 동결 비용을 줄이는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_owned_parts_no_freeze_probe_20260602`
  - Node: `perf_node_single_linux_20260602_002356_node_single_routed_owned_parts_no_freeze_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과 1:
  - `DEALER_ROUTER tcp`: 19.0/19.1/22.5%
  - `DEALER_ROUTER ws`: 19.9/21.9/30.6%
  - `DEALER_ROUTER wss`: 19.3/21.7/50.4%
  - `DEALER_ROUTER tls`: 20.1/20.9/33.1%
  - `ROUTER_ROUTER tcp`: 21.9/22.2/23.5%
  - `ROUTER_ROUTER ws`: 22.8/23.7/30.0%
  - `ROUTER_ROUTER wss`: 22.5/22.9/49.7%
  - `ROUTER_ROUTER tls`: 22.0/22.0/32.9%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_pubsub_owned_parts_no_freeze_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_002901_node_multi_pubsub_owned_parts_no_freeze_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `tcp`: 20.5/22.5/51.5%
  - `ws`: 24.5/22.5/28.0%
  - `wss`: 21.0/20.3/33.9%
  - `tls`: 22.2/21.2/28.5%
- 판정:
  - single routed의 `DEALER_ROUTER tls 1024B`는 경계선까지 올랐지만, 다른 small 미달은
    대부분 남고 일부 기존 값은 낮아졌다.
  - `MULTI_PUBSUB`은 새 통과 항목을 만들지 못했다.
  - runtime 배열 동결 제거는 public d.ts 변경은 없지만 관찰 가능한 runtime 불변성에
    영향을 줄 수 있어, 한 칸 경계선 개선만으로 유지하기에는 public contract 위험이 크다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## routed small payload Buffer copy 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER` 64/256/1024B
  - native receive snapshot의 payload `Buffer` 생성 경로
- 근거:
  - 64/256/1024B routed receive에서는 payload 복사 비용보다 `zlink_msg_t` heap allocation과
    external buffer finalizer 비용이 더 클 수 있다.
  - public `Message.data()`와 ownership 의미는 유지하고, 작은 routed message만 native Buffer
    copy로 받는 후보를 시험했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_small_copy_buffer_probe_20260602`
  - Node: `perf_node_single_linux_20260602_003406_node_single_routed_small_copy_buffer_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과 1:
  - `DEALER_ROUTER tcp`: 23.5/22.6/19.2%
  - `DEALER_ROUTER ws`: 24.0/25.8/33.0%
  - `DEALER_ROUTER wss`: 23.2/25.6/53.2%
  - `DEALER_ROUTER tls`: 24.0/24.4/35.0%
  - `ROUTER_ROUTER tcp`: 27.5/26.7/25.3%
  - `ROUTER_ROUTER ws`: 26.6/28.3/32.7%
  - `ROUTER_ROUTER wss`: 27.0/26.9/53.4%
  - `ROUTER_ROUTER tls`: 25.3/25.8/35.2%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 1024 --duration 1 --runs 5 --results-tag node_single_routed_small_copy_buffer_tcp1024_recheck_20260602`
  - Node: `perf_node_single_linux_20260602_003455_node_single_routed_small_copy_buffer_tcp1024_recheck_20260602.txt`
  - status: complete
- 결과 2:
  - `DEALER_ROUTER tcp 1024B`: 24.1%
  - `ROUTER_ROUTER tcp 1024B`: 24.8%
- 측정 3:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports wss --pattern MULTI_PUBSUB --msg-sizes 1024 --duration 1 --runs 3 --results-tag node_multi_pubsub_wss1024_routed_copy_guard_20260602`
  - Node: `perf_node_multi_linux_20260602_004103_node_multi_pubsub_wss1024_routed_copy_guard_20260602.txt`
  - status: complete
- 결과 3:
  - `MULTI_PUBSUB wss 1024B`: 33.1%
- 판정:
  - routed small의 64/256B와 일부 1024B 비율은 크게 올랐지만, 현재 계획 문서 기준으로
    새 `미달 -> 통과` 항목을 만들지는 못했다.
  - 전체 data-only receive에 적용하면 `MULTI_PUBSUB wss 1024B`가 낮아졌으므로 routed
    snapshot으로 범위를 좁혀 재검토했지만, 여전히 통과 항목 증가는 없었다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## routed message property lazy 합성 후보 기각

- 대상:
  - single routed small
  - `Message.getProperty("Routing-Id")`/`Identity`의 synthetic property 생성 경로
- 근거:
  - routed message snapshot은 public `getProperty()` 결과를 위해 매 수신마다
    `Routing-Id`/`Identity` 문자열과 properties 객체를 만든다.
  - snapshot에는 routing id Buffer만 저장하고 `getProperty()` 호출 시 문자열을 합성하면
    public 결과를 유지하면서 hot path 객체 생성을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_lazy_property_small_copy_probe_20260602`
  - Node: `perf_node_single_linux_20260602_004538_node_single_routed_lazy_property_small_copy_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 판정:
  - small payload copy 단독 후보보다 routed small 전반이 낮아졌다.
  - lazy property 합성은 hot path hidden class나 snapshot shape에 불리하게 작용하는 것으로
    보이며 통과 항목도 늘리지 못했다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT routed snapshot refCount/properties 경량화 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - SPOT routed receive의 native message snapshot 생성 경로
- 근거:
  - `create_spot_message_snapshot_value()`는 모든 SPOT receive part에
    `MESSAGE_SNAPSHOT_ALWAYS_REF_COUNT | MESSAGE_SNAPSHOT_ALWAYS_PROPERTIES`를 적용한다.
  - freshly received message의 `refCount()`는 기본값 1과 같고, routing id가 있으면
    `Routing-Id`/`Identity` synthetic property는 강제 플래그 없이도 유지된다.
  - public `.d.ts`와 receive 계약을 바꾸지 않고 native `zlink_msg_refcnt()` 호출과 강제
    properties snapshot 생성을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_spot_snapshot_light_tcp_ws_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_005621_node_multi_spot_sendsend_spot_snapshot_light_tcp_ws_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current ws 256B` client bind failed
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_spot_snapshot_light_tcp_only_20260602`
  - Node: `perf_node_multi_linux_20260602_005820_node_multi_spot_sendsend_spot_snapshot_light_tcp_only_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `tcp`: 21.7/19.4/22.5/30.6/32.9%
- 측정 3:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern SPOT --msg-sizes 131072,262144 --duration 1 --runs 3 --results-tag node_single_spot_snapshot_light_tcp_large_probe_20260602`
  - Node: `perf_node_single_linux_20260602_005946_node_single_spot_snapshot_light_tcp_large_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과 3:
  - `SPOT tcp 131072B`: 28.8%
  - `SPOT tcp 262144B`: 32.1%
- 판정:
  - `tcp 131072B`가 32.9%까지 올라왔지만 SPOT 기준 33%를 넘지 못했다.
  - single `SPOT tcp 262144B`도 32.1%로 경계에 가까웠지만 기준에 못 닿았다.
  - 새 `미달 -> 통과` 항목이 없고, tcp/ws 묶음 측정은 partial이므로 표 반영 근거로 쓸 수 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## routed message properties freeze 제거 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER` 64/256/1024B
  - `Message.fromSnapshot()`의 native properties snapshot 정규화 경로
- 근거:
  - routed receive의 synthetic `Routing-Id`/`Identity` properties 객체는 native에서 새로
    만들어지고 public API에서는 `Message.getProperty()`로만 읽힌다.
  - `Message.fromSnapshot()`이 이 객체를 다시 `Object.freeze()`하는 비용을 줄이면 public
    `.d.ts`와 receive shape를 바꾸지 않고 routed small hot path를 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_properties_no_freeze_probe_20260602`
  - Node: `perf_node_single_linux_20260602_010419_node_single_routed_properties_no_freeze_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.6/20.2/23.4%
  - `DEALER_ROUTER ws`: 20.7/22.8/31.4%
  - `DEALER_ROUTER wss`: 20.3/22.8/51.7%
  - `DEALER_ROUTER tls`: 20.7/21.7/34.3%
  - `ROUTER_ROUTER tcp`: 22.9/23.8/24.3%
  - `ROUTER_ROUTER ws`: 23.4/24.1/31.3%
  - `ROUTER_ROUTER wss`: 23.1/23.7/52.3%
  - `ROUTER_ROUTER tls`: 22.2/23.2/33.1%
- 판정:
  - routed small 값은 일부 올랐지만, 새로 `미달 -> 통과`가 된 셀은 없다.
  - `ws/wss/tls 1024B` 일부는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## multi routed large Message payload Buffer 재검토 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536B`, 인접 `131072B`
  - `Message` 재전송 시 payload 정규화 경로
- 근거:
  - 이전 `Message send normalization payload Buffer` 후보는 `MULTI_SPOT_SENDSEND` 기준에서
    새 통과를 만들지 못해 되돌렸다.
  - 남은 핵심 미달인 multi routed `tcp/ws 65536B`는 server echo가 받은 `Message`를 다시
    보내는 경로라, 같은 내부 변경이 routed large에는 효과가 있는지 별도로 확인했다.
  - native send 초기화가 현재 snapshot의 `data`만 사용하므로, public `.d.ts` 변경 없이
    `Message.payloadBuffer()`를 바로 넘기는 후보를 시험했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_message_payload_buffer_tcp_ws_large_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_011103_node_multi_routed_message_payload_buffer_tcp_ws_large_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 21.1/26.3%
  - `MULTI_DEALER_ROUTER ws`: 22.7/31.3%
  - `MULTI_ROUTER_ROUTER tcp`: 23.0/27.6%
  - `MULTI_ROUTER_ROUTER ws`: 25.2/32.3%
- 판정:
  - `ws 131072B` 두 셀은 통과지만 기존 계획 문서에서도 이미 통과였고, accepted 값보다 낮아졌다.
  - 핵심 미달인 `tcp/ws 65536B`와 `tcp 131072B`는 여전히 목표에 못 닿았다.
  - 새 `미달 -> 통과` 항목이 없고 기존 통과 셀의 수치가 낮아졌으므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## multi routed echo server Received 재사용 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536B`, 인접 `131072B`
  - Node perf routed echo server의 caller-provided `Received` storage 사용 방식
- 근거:
  - 두 routed echo server는 receive loop 내부에서 매번 `new zlink.Received()`를 만든다.
  - caller-provided storage를 쓰는 public recv API 의미와 C의 재사용 receive buffer 의미에 더
    가깝게, server별 `Received`를 하나 만들어 `router.recv(received, DontWait)`로 반복
    채우는 후보를 확인했다.
  - perf runner 변경이므로 통과 항목 증가가 없으면 최종 코드에 남기지 않는다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_reuse_received_tcp_ws_large_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_011634_node_multi_routed_reuse_received_tcp_ws_large_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 20.3/26.6%
  - `MULTI_DEALER_ROUTER ws`: 22.9/31.3%
  - `MULTI_ROUTER_ROUTER tcp`: 22.0/28.6%
  - `MULTI_ROUTER_ROUTER ws`: 24.9/31.8%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - `ws 131072B` 두 셀은 기존에도 통과였고, accepted 값보다 낮아졌다.
  - perf runner 변경은 통과 증가가 없으면 남기지 않는 원칙에 따라 되돌렸다.

## MULTI_STREAM native packet payload 고정 배열 후보 기각

- 대상:
  - `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
  - stream packet handler의 native callback payload 생성 경로
- 근거:
  - stream packet handler는 header/body 2개 packet만 JS로 넘긴다.
  - native callback payload가 매 packet마다 `std::vector<zlink_msg_t>`를 resize하므로,
    고정 배열로 바꾸면 public `.d.ts`와 stream packet handler 계약을 유지하면서
    stream small hot path의 힙 할당을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_fixed_payload_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_012508_node_multi_stream_fixed_payload_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 28.1/28.2/27.2%
  - `ws`: 20.8/21.8/20.7%
  - `wss`: 29.6/29.8/28.1%
  - `tls`: 26.3/27.5/26.4%
- 판정:
  - 일부 값은 기존 계획 문서의 accepted 값보다 소폭 높지만, 단순 one-way 기준
    최소 35%에는 못 닿아 새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## request callback 단일 reply part storage 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - request-reply callback의 native 결과 payload 생성 경로
- 근거:
  - `MULTI_SPOT_REQREP`는 request callback으로 단일 reply part를 받는다.
  - native `request_reply_callback_trampoline()`은 단일 part에도
    `std::vector<zlink_msg_t>`를 resize해서 callback payload에 보관한다.
  - public request callback 계약과 `.d.ts`를 바꾸지 않고, 단일 reply part만 stack storage에
    보관해 per-reply 힙 할당을 줄이는 후보를 시험했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_single_reply_payload_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_013158_node_multi_spot_reqrep_single_reply_payload_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 131072B`: 22.3%
  - `ws 131072B`: 22.3%
- 판정:
  - 두 셀 모두 SPOT 기준 33%에 못 닿아 새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB empty routingId property 생략 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - subscribed message native raw object 생성 경로
- 근거:
  - 일반 PUBSUB receive는 routing id가 없는 경우가 대부분이다.
  - `create_subscribed_value()`는 이 경우에도 매번 `routingId: null` 속성을 만든다.
  - TypeScript materializer는 `raw.routingId ?? null`로 같은 public 값을 만들 수 있으므로,
    empty routing id일 때 속성 생성을 생략해 hot path object shape 비용을 줄일 수 있는지
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_omit_empty_routing_id_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_013937_node_multi_pubsub_omit_empty_routing_id_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 21.2/23.8/51.0/14.8/21.7%
  - `ws`: 24.7/20.1/26.5/29.6/33.0%
  - `wss`: 21.1/19.9/35.2/38.3/40.5%
  - `tls`: 22.0/21.3/30.3/32.4/35.2%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과였던 `MULTI_PUBSUB ws 65536B`가 29.6%로 미달권에 내려갈 수
    있어 회귀 후보로 본다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀가 있는 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## spotRequestSpot 단일 payload stack fast path 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - request submit의 native payload builder 경로
- 근거:
  - `MULTI_SPOT_REQREP` client는 단일 payload로 `spot.requestToSpot(...).message(...).submit(...)`
    경로를 반복한다.
  - native `spot_request_spot()`는 단일 payload도 `build_msg_vector_or_single()`로
    `std::vector<zlink_msg_t>`를 만든다.
  - public request API를 바꾸지 않고, `spot_request_spot()` 단일 payload에서만 stack
    `zlink_msg_t`를 쓰는 후보를 확인했다.
- 검증:
  - `npm run rebuild-native`: 실패
  - 실패 원인: `addon_spot.cc`에서 `init_msg_from_value(...)`를 직접 사용할 수 없다.
- 판정:
  - 이 후보를 유지하려면 native message builder helper를 translation unit 밖으로 새로 공유해야 한다.
  - 작은 hot path 후보 하나를 위해 native helper 경계를 넓히는 것은 public contract는 아니더라도
    변경 범위와 리스크가 커진다.
  - 빌드 단계에서 통과하지 못했으므로 측정으로 진행하지 않고 되돌렸다.

## spotRequestSpot 단일 payload stack fast path 재검토 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - request submit의 native payload builder 경로
- 근거:
  - `init_msg_from_value(...)`는 실제 전역 native symbol이고, `addon_common_api.h` 선언만 빠져
    있어 이전 후보가 컴파일되지 않았다.
  - public API를 바꾸지 않고 native 내부 선언을 맞춘 뒤, `spot_request_spot()` 단일 payload에서
    stack `zlink_msg_t`를 쓰면 request submit 비용을 줄일 수 있는지 재확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_request_stack_payload_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_014450_node_multi_spot_reqrep_request_stack_payload_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 131072B`: 23.5%
  - `ws 131072B`: 21.8%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - `tcp`는 소폭 올랐지만 SPOT 기준 33%에 못 닿았고, `ws`는 직전 request callback storage
    후보보다 낮아졌다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀가 있는 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT routed recv 단일 part vector 우회 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024/131072B`
  - `spotRecvRouted` native receive 경로
- 근거:
  - `MULTI_SPOT_SENDSEND`는 routed single-part receive가 hot path다.
  - 기존 `spot_recv_routed()`는 단일 part도 `spot_recv_parts()`를 통해
    `std::vector<zlink_msg_t>`에 move한 뒤 JS 배열을 만든다.
  - public receive shape와 `.d.ts`를 바꾸지 않고, `has_more == ZLINK_PART_FINAL`인 경우
    stack `zlink_msg_t`에서 바로 snapshot 배열을 만들면 vector 할당과 move를 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_routed_recv_single_part_fast_path_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_015608_node_multi_spot_sendsend_routed_recv_single_part_fast_path_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.4/18.1/18.9/21.3%
  - `ws`: 19.1/19.5/21.3/30.7%
  - `wss`: 17.8/18.1/20.6/54.5%
  - `tls`: 18.9/19.5/19.8/43.4%
- 판정:
  - `wss 131072B`, `tls 131072B`는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - small payload 미달 셀은 여전히 SPOT 기준 33%에 못 닿아 새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## owned routing id 내부 검증 생략 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `RoutingId.fromOwnedBuffer()` materialize 경로
- 근거:
  - native receive raw 객체가 넘긴 routing id는 이미 Node `Buffer`이고 길이도 native가 보장한다.
  - `.d.ts`에서 제거되는 내부 helper `RoutingId.fromOwnedBuffer()`의 `Buffer.isBuffer`와 길이 검사를
    생략하면 routed small hot path의 JS 검증 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_trusted_owned_routing_id_probe_20260602`
  - Node: `perf_node_single_linux_20260602_020147_node_single_routed_trusted_owned_routing_id_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 20.0/19.9/23.1%
  - `DEALER_ROUTER ws`: 20.4/23.0/31.6%
  - `DEALER_ROUTER wss`: 19.7/22.3/51.2%
  - `DEALER_ROUTER tls`: 20.9/21.6/33.5%
  - `ROUTER_ROUTER tcp`: 23.1/23.4/24.1%
  - `ROUTER_ROUTER ws`: 22.8/24.4/31.8%
  - `ROUTER_ROUTER wss`: 22.7/23.7/51.3%
  - `ROUTER_ROUTER tls`: 21.9/22.7/33.2%
- 판정:
  - `ws/wss/tls 1024B` 일부는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - `tcp 64/256/1024B`, `ws 64/256B`, `wss 64/256B`, `tls 64/256B` 미달은 해소되지 않았다.
  - internal helper의 방어 검사를 제거할 만큼의 통과 증가가 없으므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## router raw spotRid null 생략 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER` 64/256/1024B
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` `tcp/ws 65536/131072B`
  - native `create_router_recv_message_value()` raw 객체 생성 경로
- 근거:
  - 일반 routed receive는 SPOT route가 아니므로 `spotRid`가 대부분 `null`이다.
  - JS materializer는 `raw.spotRid ?? null`로 같은 public 값을 만들 수 있으므로,
    native raw 객체에서 null `spotRid` 속성 생성을 생략하면 public `.d.ts`를 바꾸지 않고
    routed receive object 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_omit_null_spotrid_probe_20260602`
  - Node: `perf_node_single_linux_20260602_020746_node_single_routed_omit_null_spotrid_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과 1:
  - `DEALER_ROUTER tcp`: 20.5/20.9/24.4%
  - `DEALER_ROUTER ws`: 20.7/23.2/33.0%
  - `DEALER_ROUTER wss`: 20.5/23.2/52.5%
  - `DEALER_ROUTER tls`: 21.4/22.1/34.4%
  - `ROUTER_ROUTER tcp`: 23.9/24.4/24.8%
  - `ROUTER_ROUTER ws`: 23.7/24.7/32.1%
  - `ROUTER_ROUTER wss`: 23.1/24.2/52.7%
  - `ROUTER_ROUTER tls`: 22.4/23.3/34.0%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_omit_null_spotrid_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_021138_node_multi_routed_omit_null_spotrid_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `MULTI_DEALER_ROUTER tcp`: 20.0/26.1%
  - `MULTI_DEALER_ROUTER ws`: 22.1/31.0%
  - `MULTI_ROUTER_ROUTER tcp`: 21.6/26.4%
  - `MULTI_ROUTER_ROUTER ws`: 24.5/31.3%
- 판정:
  - single routed의 1024B 일부와 multi routed `ws 131072B`는 통과지만 기존 계획 문서에서도
    이미 통과였던 셀이다.
  - 핵심 미달인 single routed small 대부분과 multi routed `tcp/ws 65536B`,
    `tcp 131072B`는 해소되지 않았다.
  - goal 기준상 새 `미달 -> 통과` 항목이 없으므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## singlePartOrThrow 직접 길이 검사 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - reusable `TopicMessage`의 단일 part 확인 경로
- 근거:
  - PUBSUB/SPOT 계열 perf hot path는 수신마다 `received.singlePartOrThrow().data()`를 호출한다.
  - public 의미는 그대로 두고 `singlePartOrThrow()` 내부에서 `isSinglePart()` 메서드 호출을
    직접 `this.parts.length !== 1` 검사로 바꾸면 JS 호출 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_single_part_direct_length_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_021828_node_multi_pubsub_single_part_direct_length_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.0/22.4/47.2/14.9/21.7%
  - `ws`: 23.1/20.7/25.1/31.2/32.3%
  - `wss`: 20.4/19.1/33.7/37.0/37.9%
  - `tls`: 21.0/20.8/28.4/31.7/31.9%
- 판정:
  - `tcp 1024B`, `wss 65536/131072B`는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - `tcp 64/256/65536/131072B`, `ws` 전반, `wss 64/256B`, `tls` 전반의 미달은 해소되지 않았다.
  - 새 `미달 -> 통과` 항목이 없으므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## owned routing id freeze 생략 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `RoutingId.fromOwnedBuffer()` materialize 경로
- 근거:
  - routed receive materializer는 매 수신마다 native가 넘긴 routing id `Buffer`를 내부
    `RoutingId.fromOwnedBuffer()`로 감싼다.
  - public `RoutingId.from()`/`fromHex()` 경로는 그대로 freeze하고, `.d.ts`에서 제거되는
    내부 receive helper에서만 `Object.freeze(this)`를 생략하면 객체 생성 비용을 줄일 수 있는지
    확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_from_owned_no_freeze_probe_20260602`
  - Node: `perf_node_single_linux_20260602_022339_node_single_routed_from_owned_no_freeze_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 20.1/20.0/23.2%
  - `DEALER_ROUTER ws`: 20.3/22.9/31.7%
  - `DEALER_ROUTER wss`: 20.0/22.2/51.8%
  - `DEALER_ROUTER tls`: 20.7/21.5/33.6%
  - `ROUTER_ROUTER tcp`: 23.1/23.7/24.5%
  - `ROUTER_ROUTER ws`: 22.9/24.2/31.8%
  - `ROUTER_ROUTER wss`: 22.8/23.7/51.1%
  - `ROUTER_ROUTER tls`: 22.0/22.6/33.3%
- 판정:
  - `wss/tls 1024B` 일부는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없고, 직전 routed raw-shape 계열 후보보다 낮은 값이 많다.
  - receive된 `RoutingId`의 freeze 여부는 public `.d.ts`에 드러나지 않더라도 런타임에서
    관찰 가능한 객체 동작이므로, 통과 증가 없이 유지할 이유가 없다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## Message.fromSnapshot data-only fast init 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - `Message.fromSnapshot()`의 data-only receive materialize 경로
- 근거:
  - native snapshot이 `{ data }`만 가진 data-only 메시지인 경우에도 `Message.fromSnapshot()`은
    `initialize()`를 호출하면서 refCount 기본값 처리와 properties/metadata 정규화 함수를 지난다.
  - public `Message` API와 native snapshot shape는 유지하고, data-only snapshot에서만 내부 필드를
    직접 채우면 receive hot path의 JS 호출 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `dist/index.d.ts` diff 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_message_snapshot_fast_init_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_023107_node_multi_pubsub_message_snapshot_fast_init_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.0/22.7/48.1/14.8/21.6%
  - `ws`: 23.7/22.3/27.2/30.8/31.9%
  - `wss`: 20.7/19.5/33.6/37.5/38.9%
  - `tls`: 21.3/20.8/29.1/33.1/36.3%
- 판정:
  - `tcp 1024B`, `wss 65536/131072B`, `tls 131072B`는 통과지만 기존 계획 문서에서도 이미
    통과였던 셀이다.
  - `tls 65536B`는 33.1%까지 올랐지만 기존 계획 문서 기준으로도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없으므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single PUBSUB TopicMessage._replace no-op close loop 제거 후보 채택

- 대상:
  - single `PUBSUB`
  - `wss 64B`
  - reusable `TopicMessage` subscribe hot path
- 근거:
  - `TopicMessage._replace()`는 caller-provided storage 재사용 시 이전 parts를 순회하며
    `Message.close()`를 호출한다.
  - 현재 `Message.close()`는 no-op이고, 같은 no-op close loop 제거는 `Received._replace()`에서도
    routed receive 개선으로 채택됐다.
  - 이전 multi PUBSUB 측정에서는 새 통과 항목이 없어 되돌렸지만, single `PUBSUB wss 64B`는
    35% 기준 바로 아래라 single 경계 셀을 별도로 확인했다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports wss --pattern PUBSUB --msg-sizes 64 --duration 1 --runs 3 --results-tag node_single_pubsub_topic_replace_no_close_wss64_probe_20260602`
  - Node: `perf_node_single_linux_20260602_023659_node_single_pubsub_topic_replace_no_close_wss64_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260601_140755_node_single_simple64_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - `PUBSUB wss 64B`: Node 473.449 Kmsg/s, C 1235.792 Kmsg/s, C 대비 38.3%
- 판정:
  - 계획 문서의 single `PUBSUB wss 64B`가 `미달(34.7%)`에서 `통과(38.3%)`로 올랐다.
  - public `.d.ts` 변경 없이 bindings 내부 no-op close 순회만 제거하므로 유지한다.

## single routed materializeParts native 배열 재사용 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - receive materializer의 `Message[]` 구성 경로
- 근거:
  - native raw 객체는 이미 `parts` 배열을 만들어 JS로 넘긴다.
  - `materializeParts()`가 별도 `Message[]`를 새로 만들지 않고 native 배열의 각 요소를
    `Message`로 교체해 반환하면 public receive shape를 유지하면서 배열 할당을 줄일 수 있는지
    확인했다.
  - 이전 multi PUBSUB에서는 새 통과 항목이 없어 기각됐지만, single routed small은 별도로
    확인되지 않아 targeted 측정을 수행했다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_materialize_parts_inplace_probe_20260602`
  - Node: `perf_node_single_linux_20260602_024317_node_single_routed_materialize_parts_inplace_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.8/19.9/22.7%
  - `DEALER_ROUTER ws`: 20.2/23.1/31.7%
  - `DEALER_ROUTER wss`: 20.2/22.7/51.1%
  - `DEALER_ROUTER tls`: 20.6/21.8/34.3%
  - `ROUTER_ROUTER tcp`: 23.1/23.6/24.4%
  - `ROUTER_ROUTER ws`: 23.5/24.4/31.6%
  - `ROUTER_ROUTER wss`: 23.0/23.9/52.1%
  - `ROUTER_ROUTER tls`: 22.6/23.0/33.6%
- 판정:
  - 일부 routed small 비율은 기존 계획 문서 값보다 올랐지만, 새 `미달 -> 통과` 항목은 없다.
  - `ws/wss/tls 1024B` 통과 항목은 기존 계획 문서에서도 이미 통과였던 셀이다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## request messagesFromNativeBuffers 단일 reply fast path 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - request callback의 JS reply part materialize 경로
- 근거:
  - native callback 단일 reply part storage 후보와 request submit stack fast path 후보는 이미
    새 통과 항목을 만들지 못했다.
  - JS 경로에서는 `messagesFromNativeBuffers()`가 단일 reply에도 `Array.map()` callback을
    사용하므로, 단일 reply fast path와 for-loop를 적용하면 public callback 계약을 유지한 채
    wrapper 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_messages_from_native_single_fastpath_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_024527_node_multi_spot_reqrep_messages_from_native_single_fastpath_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 131072B`: 22.8%
  - `ws 131072B`: 21.9%
- 판정:
  - 두 셀 모두 SPOT 기준 33%에 못 닿아 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서의 `tcp 22.3%`, `ws 21.8%`와 같은 변동 범위라 유지할 근거가 부족하다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single routed singlePartOrThrow 직접 길이 검사 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `received.singlePartOrThrow().data()` drain 경로
- 근거:
  - multi PUBSUB 기준의 직접 길이 검사 후보는 새 통과 항목이 없어 기각됐다.
  - single routed small은 여전히 핵심 미달 묶음이고, drain 경로에서 매 수신마다
    `singlePartOrThrow()`가 호출되므로 별도 targeted 측정으로 확인했다.
  - public 의미는 유지하고 내부에서 `isSinglePart()` 메서드 호출 대신
    `this.parts.length !== 1`을 직접 검사했다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_single_part_direct_length_probe_20260602`
  - Node: `perf_node_single_linux_20260602_025044_node_single_routed_single_part_direct_length_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.7/19.9/23.0%
  - `DEALER_ROUTER ws`: 20.6/23.0/31.8%
  - `DEALER_ROUTER wss`: 20.1/22.3/51.8%
  - `DEALER_ROUTER tls`: 20.6/21.4/34.1%
  - `ROUTER_ROUTER tcp`: 22.8/23.4/23.8%
  - `ROUTER_ROUTER ws`: 23.3/24.3/31.2%
  - `ROUTER_ROUTER wss`: 22.6/23.9/51.7%
  - `ROUTER_ROUTER tls`: 21.7/22.6/33.6%
- 판정:
  - `ws 1024B`는 31%대까지 올랐지만 routed 기준 33%에는 못 닿았다.
  - `wss/tls 1024B` 통과 항목은 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single routed blocking router recv 단일 part native fast path 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - blocking `router.recv()` native receive 경로
- 근거:
  - 이전 multi native receive 단일 part fast path 후보는 일부 통과 증가가 있었지만
    multi routed small 회귀 위험이 있어 기각됐다.
  - 이번에는 multi server가 주로 쓰는 no-wait 경로를 건드리지 않고, single-context drain의
    blocking `router.recv()` 경로에서만 단일 part일 때 `std::vector<zlink_msg_t>`를 우회하는
    좁은 후보를 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_blocking_recv_single_part_native_probe_20260602`
  - Node: `perf_node_single_linux_20260602_025530_node_single_routed_blocking_recv_single_part_native_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 20.1/20.4/22.9%
  - `DEALER_ROUTER ws`: 20.5/22.8/31.9%
  - `DEALER_ROUTER wss`: 19.5/22.7/51.5%
  - `DEALER_ROUTER tls`: 20.6/21.5/33.8%
  - `ROUTER_ROUTER tcp`: 22.7/23.6/24.1%
  - `ROUTER_ROUTER ws`: 22.5/24.8/31.5%
  - `ROUTER_ROUTER wss`: 23.0/23.6/51.2%
  - `ROUTER_ROUTER tls`: 21.8/22.6/33.3%
- 판정:
  - `ws 1024B`는 여전히 routed 기준 33%에 못 닿았다.
  - `wss/tls 1024B` 통과 항목은 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## multi routed large-only no-wait router recv native fast path 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536/131072B`
  - no-wait `router.recv()` native receive 경로
- 근거:
  - 이전 multi native receive 단일 part fast path 후보는 small 회귀 위험 때문에 기각됐다.
  - 이번에는 small을 제외하고 `zlink_msg_size(first_part) >= 65536`인 단일 part에서만
    `std::vector<zlink_msg_t>` move를 우회해 multi routed 65536B 미달을 해소할 수 있는지
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_large_only_recv_fastpath_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_030110_node_multi_routed_large_only_recv_fastpath_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 20.2/24.4%
  - `MULTI_DEALER_ROUTER ws`: 22.2/30.2%
  - `MULTI_ROUTER_ROUTER tcp`: 21.8/27.3%
  - `MULTI_ROUTER_ROUTER ws`: 24.3/32.3%
- 판정:
  - 핵심 미달인 `tcp/ws 65536B`는 모두 multi routed 기준 30%에 못 닿았다.
  - 기존 계획 문서에서 통과였던 `tcp 131072B` 셀들이 미달권으로 내려갈 수 있어 회귀 위험이 있다.
  - 새 `미달 -> 통과` 항목이 없고 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## Received empty parts singleton 후보 기각

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536/131072B`
  - public `new Received()` 빈 envelope 초기화 경로
- 근거:
  - multi routed echo server는 receive loop 안에서 매번 `new zlink.Received()`를 만든다.
  - 기본 생성자는 `super([])`를 통해 빈 배열을 매번 `slice()`하고 `Object.freeze()`한다.
  - public `parts` shape는 빈 배열 그대로 유지하면서 frozen empty array singleton을 재사용하면
    receive envelope 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_empty_parts_singleton_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_030633_node_multi_routed_empty_parts_singleton_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 20.0/26.0%
  - `MULTI_DEALER_ROUTER ws`: 22.7/30.3%
  - `MULTI_ROUTER_ROUTER tcp`: 21.7/27.8%
  - `MULTI_ROUTER_ROUTER ws`: 25.1/32.6%
- 판정:
  - 핵심 미달인 `tcp/ws 65536B`는 모두 multi routed 기준 30%에 못 닿았다.
  - 기존 계획 문서에서 통과였던 `tcp 131072B` 셀들이 미달권으로 내려갈 수 있어 회귀 위험이 있다.
  - 새 `미달 -> 통과` 항목이 없고 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## socket recv raw hasMore property 생략 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER` `tcp/ws/wss/tls 64/256/1024B`
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` `tcp/ws 65536/131072B`
  - native `create_recv_message_value()` raw object 생성 경로
- 근거:
  - `hasMore`는 native receive raw 객체에 항상 `false`로 설정되지만 TypeScript materializer와
    테스트에서 사용하지 않는다.
  - public `.d.ts` 표면을 바꾸지 않고 private raw 객체의 불필요한 property set 비용만 줄이는
    후보로 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령 1: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_omit_hasmore_probe_20260602`
  - Node 1: `perf_node_single_linux_20260602_031423_node_single_routed_omit_hasmore_probe_20260602.txt`
  - 명령 2: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_omit_hasmore_probe_20260602`
  - Node 2: `perf_node_multi_linux_20260602_031817_node_multi_routed_omit_hasmore_probe_20260602.txt`
  - status: complete
- 결과:
  - single tcp routed small은 기존 표보다 일부 올랐지만 `DEALER_ROUTER tcp 64/256/1024B`가
    21.9/22.8/24.5%, `ROUTER_ROUTER tcp 64/256/1024B`가 24.7/25.4/26.2%로 여전히
    기준에 못 닿았다.
  - multi routed large는 `MULTI_DEALER_ROUTER tcp/ws 65536B` 23.456/24.751 Kops/s,
    `MULTI_ROUTER_ROUTER tcp/ws 65536B` 26.650/25.136 Kops/s로 기존 accepted 값과 같은
    변동 범위이며 새 통과 항목이 없다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없고 multi large 일부는 기존 accepted 값보다 낮다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## MULTI_PUBSUB socketTryPublish Buffer 우선 판별 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - native `socketTryPublish()` 단일 Buffer payload 경로
- 근거:
  - `publish(...).flags(DontWait).submit()` hot path는 단일 `Buffer` payload가 대부분이다.
  - 기존 native 경로는 배열 여부를 먼저 확인한 뒤 Buffer를 판별하므로, Buffer를 먼저 처리하면
    public API와 runner 의미를 바꾸지 않고 N-API 타입 체크 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_try_publish_buffer_first_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_032814_node_multi_pubsub_try_publish_buffer_first_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 20.3/23.2/50.5/14.8/21.9%
  - `ws 64/256/1024/65536/131072B`: 24.2/21.3/27.9/32.5/34.9%
  - `wss 64/256/1024/65536/131072B`: 20.8/17.6/33.8/37.9/38.5%
  - `tls 64/256/1024/65536/131072B`: 22.9/21.6/29.2/32.8/35.6%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과권이던 `MULTI_PUBSUB ws 131072B`와 `tls 65536B`가 경계 아래로
    내려갈 위험이 있어 회귀 후보로 본다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT recvRouted 중간 raw 객체 생략 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp/ws 64/256/1024/65536/131072B`
  - TypeScript `Spot.recvRouted()` materialize 경로
- 근거:
  - native `spotRecvRouted()` raw 객체는 이미 `sourceRid`, `spotRid`, `requestSeq`, `parts`를
    가지고 있다.
  - 기존 `Spot.recvRouted()`는 매 수신마다 `{ routingId: raw.sourceRid, ... }` 중간 객체를
    새로 만들어 `materializeReceivedInto()`에 넘긴다.
  - materializer가 `sourceRid`를 직접 읽게 하면 public API와 native raw shape는 유지하면서
    SPOT routed receive의 중간 객체 할당을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령 1: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_direct_raw_materialize_probe_20260602`
  - Node 1: `perf_node_multi_linux_20260602_033437_node_multi_spot_sendsend_direct_raw_materialize_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current tcp 65536B` client timeout
  - 명령 2: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_direct_raw_materialize_small_probe_20260602`
  - Node 2: `perf_node_multi_linux_20260602_033710_node_multi_spot_sendsend_direct_raw_materialize_small_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_SPOT_SENDSEND current tcp 64B` client timeout
- 판정:
  - 두 측정 모두 partial이므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - timeout 회귀가 반복되어 후보를 안정적인 개선으로 볼 수 없다.
  - goal 기준상 실패 후보는 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single SPOT publish Buffer stack fast path 후보 기각

- 대상:
  - single `SPOT`
  - `tcp 131072/262144B`
  - native `spotPublish()` 단일 Buffer publish 경로
- 근거:
  - `spotPublish()`는 단일 Buffer payload에서도 `std::vector<zlink_msg_t>`를 만들고 `resize(1)`로
    native message storage를 준비한다.
  - public 계약과 perf runner 의미를 바꾸지 않고 단일 Buffer payload만 stack `zlink_msg_t`로 보내면
    SPOT publish large 경로의 native heap/vector 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern SPOT --msg-sizes 131072,262144 --duration 1 --runs 3 --results-tag node_single_spot_publish_buffer_stack_tcp_large_probe_20260602`
  - Node: `perf_node_single_linux_20260602_034245_node_single_spot_publish_buffer_stack_tcp_large_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `SPOT tcp 131072B`: 28.9%
  - `SPOT tcp 262144B`: 30.5%
- 판정:
  - 두 셀 모두 SPOT 기준 33%에 못 닿아 새 `미달 -> 통과` 항목이 없다.
  - `262144B`는 기존 계획 문서의 31.0%보다 낮아 회귀 위험도 있다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT routed raw requestSeq=0 생략 후보 기각

- 대상:
  - `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `create_spot_routed_value()` raw 객체 생성 경로
- 근거:
  - 일반 send-send 수신은 `requestSeq=0`이고 public `Received.requestSeq`는 materializer에서
    reply 불가 의미의 `null`로 유지된다.
  - raw 객체의 `requestSeq`는 internal shape이므로 0일 때 BigInt 생성과 property set을 생략하면
    public `.d.ts`와 contract를 바꾸지 않고 SPOT routed receive 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_omit_zero_requestseq_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_034525_node_multi_spot_sendsend_omit_zero_requestseq_probe_20260602.txt`
  - status: partial
- 결과:
  - `MULTI_SPOT_SENDSEND current tcp 64B`가 3회 모두 `no measured messages`로 실패했다.
  - 세 번째 run에서는 core `fast_mutex.hpp:98` invalid argument도 출력됐다.
- 판정:
  - 측정이 partial이라 계획 문서 표 반영 근거로 사용할 수 없다.
  - 첫 targeted transport/size에서 메시지 측정 실패가 반복되어 안정성 회귀 후보로 본다.
  - goal 기준상 실패 후보는 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single routed native 단일 part send 직접 호출 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `socketSendRouting*` 단일 메시지 송신 경로
- 근거:
  - public `send(routingId).message(...).flags(...).submit()` 계약과 perf runner는 그대로 두고,
    native `socket_send_routing()`/`socket_try_send_routing()`에서 단일 메시지를 보낼 때
    generic `send_parts_rid(..., part_count=1)` loop 대신 `zlink_send_part_rid(..., ZLINK_PART_FINAL)`를
    직접 호출하면 routed small 송신 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_send_single_direct_native_probe_20260602`
  - Node: `perf_node_single_linux_20260602_035134_node_single_routed_send_single_direct_native_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.8/19.8/22.7%
  - `DEALER_ROUTER ws`: 20.0/22.8/31.1%
  - `DEALER_ROUTER wss`: 20.0/22.3/51.0%
  - `DEALER_ROUTER tls`: 20.5/21.5/33.9%
  - `ROUTER_ROUTER tcp`: 22.9/23.4/24.5%
  - `ROUTER_ROUTER ws`: 23.1/23.9/31.2%
  - `ROUTER_ROUTER wss`: 22.8/23.6/50.8%
  - `ROUTER_ROUTER tls`: 22.1/22.5/33.5%
- 판정:
  - 핵심 미달인 `tcp/ws/wss/tls 64/256B`와 `tcp 1024B`는 여전히 기준에 못 닿는다.
  - `wss/tls 1024B` 통과 항목은 기존 계획 문서에서도 이미 통과였던 셀이다.
  - `ws 1024B`는 수치가 조금 올랐지만 기존 계획 문서에서도 이미 통과권으로 기록된 셀이라
    새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB native subscribe topic stack buffer 후보 기각

- 대상:
  - `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - native `socketSubscribeMessage()`/`socketTrySubscribeMessage()` topic buffer 준비 경로
- 근거:
  - PUBSUB receive hot path는 수신마다 `std::vector<char>(256)`로 topic buffer를 만든다.
  - perf topic은 256B 이하라 stack buffer로 충분하므로, topic이 더 길 때만 heap vector로
    확장하면 public `subscribe()` 계약과 runner 의미를 바꾸지 않고 native heap 비용을 줄일
    수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_stack_topic_buffer_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_035934_node_multi_pubsub_stack_topic_buffer_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.3/23.5/49.8/14.9/21.4%
  - `ws`: 23.6/20.8/27.8/29.4/32.8%
  - `wss`: 21.6/15.2/33.7/37.1/38.6%
  - `tls`: 22.2/21.5/29.7/31.9/36.6%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과권이던 `MULTI_PUBSUB ws 65536B`, `ws 131072B`, `tls 65536B`가
    미달권으로 내려갈 위험이 있다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## routed raw 객체 napi_define_properties 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `create_recv_message_value()`/`create_router_recv_message_value()` raw 객체 생성 경로
- 근거:
  - raw 객체 shape 자체를 줄이는 후보는 이미 기각됐지만, shape는 유지하면서 `parts`,
    `routingId`, `hasMore`, `requestSeq`, `spotRid` property 설정을 `napi_define_properties()`로
    묶으면 N-API 호출 비용을 줄일 수 있는지 확인했다.
  - public `.d.ts`와 materializer 입력 shape는 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_define_properties_probe_20260602`
  - Node: `perf_node_single_linux_20260602_040501_node_single_routed_define_properties_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 18.8/19.0/21.7%
  - `DEALER_ROUTER ws`: 19.5/21.7/29.8%
  - `DEALER_ROUTER wss`: 19.3/21.2/48.9%
  - `DEALER_ROUTER tls`: 19.6/20.8/32.1%
  - `ROUTER_ROUTER tcp`: 21.9/22.3/23.8%
  - `ROUTER_ROUTER ws`: 22.2/23.3/30.4%
  - `ROUTER_ROUTER wss`: 21.6/22.2/49.2%
  - `ROUTER_ROUTER tls`: 21.3/21.7/32.7%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 accepted routed small 값보다 전반적으로 낮아졌고 `tls 1024B`도 미달권으로 내려갈
    수 있어 회귀 후보로 본다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## 2026-06-02 Node 남은 후보 재검토 메모

- 현재 문서 기준:
  - single: `28/144 = 19.4%` 미달
  - multi: `44/156 = 28.2%` 미달
- 현재 유지 중인 내부 개선:
  - native routing id Buffer 재복사 제거
  - requestSeq가 없거나 `0n`인 routed recv의 reply context 생성 제거
  - message materializer single-part fast path
  - data-only receive에서 stub property probe 제거
  - reusable `Received`/`TopicMessage`의 no-op close loop 제거
- 정리:
  - `bindings/node/native/src/addon_core.cc`에 남아 있던 측정 근거 없는 property set 순서 변경은
    성능 후보가 아니므로 되돌렸다.
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 남은 핵심 묶음 검토:
  - single routed small:
    - routing id cache, owned routing id freeze 생략, native raw object shape 축소,
      `hasMore`/`spotRid`/`requestSeq` 생략, `napi_define_properties`, native single-part send,
      materialize parts 배열 재사용, `singlePartOrThrow()` 직접 길이 검사, parts freeze 제거는
      모두 complete 또는 partial 측정으로 기각됐다.
    - 현재 public `Received`/`Message`/`RoutingId` 객체 materialize 경계를 유지하는 범위에서
      새 통과 항목을 만들 만한 남은 저위험 내부 후보를 찾지 못했다.
  - multi routed `tcp/ws 65536B`:
    - native receive 단일 part fast path, pending payload/Message 재검토, raw object shape 생략,
      server receive 재사용 후보가 이미 기각됐다.
    - 남은 큰 차이는 perf server의 pending payload 보관 또는 public routed raw send/borrowed
      send context 성격에 가까운데, 이는 perf runner 변경 또는 public contract 확장으로 이어져
      현재 perf 원칙에 맞지 않는다.
  - `MULTI_STREAM`:
    - raw stream send, raw packet handler, peer-index routing id cache, packet routing id
      `fromOwnedBuffer`, native packet payload 고정 배열 후보를 이미 검토했다.
    - peer-index 캐시는 native callback lifetime 문제로 `double free or corruption`이 발생해
      안전하지 않다.
    - 현재 plan 표는 runner의 pending queue head-index 개선 complete report 기준이며,
      추가 raw packet/send 후보는 public 또는 perf runner 전용 우회 성격이 강해 새로 적용하지 않는다.
  - `MULTI_PUBSUB`:
    - raw subscribe, topic storage reuse, empty routingId property 생략, tryPublish Buffer 우선 판별,
      native subscribe topic stack buffer, `TopicMessage._replace()` no-op close loop를 검토했다.
    - topic 문자열 캐시는 public `TopicMessage.topic` 값을 유지하려면 native persistent string cache가
      필요하고, per-env 수명/해제 정책 없이 넣으면 장기 실행 프로세스에서 메모리 보존 문제가 생긴다.
      perf 전용 전역 cache는 bindings library 내부 개선으로 보기 어렵기 때문에 후보에서 제외한다.
  - `MULTI_SPOT_SENDSEND`/`MULTI_SPOT_REQREP`:
    - raw routed receive/send, raw routing id send-context, single payload stack fast path,
      request callback 단일 reply storage, request messagesFromNativeBuffers fast path,
      `requestSeq=0` raw property 생략을 이미 검토했다.
    - 남은 개선은 `Received.send()` public builder 우회나 private raw routed helper 성격이 강하고,
      optimization guard가 금지하는 방향이므로 현재 제약에서는 유지 가능한 후보로 보지 않는다.
- 다음 반복 방향:
  - 새 코드를 넣기 전에 같은 후보를 반복하지 말고, 위 목록에 없는 실제 bindings 내부 경로만
    선택한다.
  - 후보가 perf runner 또는 public contract 변경 없이는 성립하지 않으면 구현하지 말고 그 이유를
    이 log에 남긴다.

## routing id parse memset 제거 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws 64/256/1024B`
  - native `parse_routing_id()`/`parse_routing_id_value()` 송신 hot path
- 근거:
  - Node native 송신 경로는 매 전송마다 JS `RoutingId` Buffer를 `zlink_routing_id_t`로 복사한다.
  - core 사용 경로는 `routing_id.size`와 `data[0..size)`만 읽으므로, 전체 struct `memset()`을
    생략하면 public contract와 perf runner 의미를 바꾸지 않고 송신 경로의 작은 고정 비용을 줄일
    수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_parse_rid_no_memset_probe_20260602`
  - Node: `perf_node_single_linux_20260602_041644_node_single_routed_parse_rid_no_memset_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.8/19.5/22.8%
  - `DEALER_ROUTER ws`: 20.2/22.6/31.4%
  - `ROUTER_ROUTER tcp`: 23.1/24.0/24.0%
  - `ROUTER_ROUTER ws`: 23.6/24.1/31.3%
- 판정:
  - throughput 자체는 일부 기존 값보다 올랐지만, targeted 범위에서 새 `미달 -> 통과` 항목은 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## receive routing id sized copy 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws 64/256/1024B`
  - native `copy_routing_id()` receive hot path
- 근거:
  - Node native receive 경로는 core에서 받은 `zlink_routing_id_t`를 JS raw 객체로 만들기 전에
    전체 struct 크기만큼 복사한다.
  - public routing id 의미는 `size`와 `data[0..size)`이므로, 실제 길이만 복사하면 public
    contract와 perf runner 의미를 바꾸지 않고 receive-side 고정 복사 비용을 줄일 수 있는지
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_copy_rid_sized_probe_20260602`
  - Node: `perf_node_single_linux_20260602_042005_node_single_routed_copy_rid_sized_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp`: 19.5/20.1/23.0%
  - `DEALER_ROUTER ws`: 20.6/22.8/31.1%
  - `ROUTER_ROUTER tcp`: 22.9/23.0/23.9%
  - `ROUTER_ROUTER ws`: 23.0/24.3/31.6%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 일부 셀은 직전 parse 후보보다 낮고, 기존 plan의 통과권 셀도 추가로 만들지 못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## routing id parse/copy 결합 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `ws 1024B`
  - native routing id 송신 parse와 receive copy 경로
- 근거:
  - `routing id parse memset 제거`와 `receive routing id sized copy`는 각각 통과 항목을 만들지 못했다.
  - 두 후보가 같은 routed small hot path의 송신/수신 양쪽 고정 비용을 줄이므로, 경계 셀인
    `ws 1024B`에서 결합했을 때 통과권에 닿는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 1024 --duration 1 --runs 3 --results-tag node_single_routed_parse_copy_rid_combined_ws1024_probe_20260602`
  - Node: `perf_node_single_linux_20260602_042213_node_single_routed_parse_copy_rid_combined_ws1024_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER ws 1024B`: 31.9%
  - `ROUTER_ROUTER ws 1024B`: 31.3%
- 판정:
  - 결합해도 새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## large payload zero-copy send 후보 제외

- 대상:
  - multi routed `tcp/ws 65536B`
  - Node native `init_msg_from_value()` / `init_msg_from_bytes()` 송신 payload 생성 경로
- 검토:
  - 현재 Node native 송신 경로는 JS `Buffer`/`MessageSnapshot.data`를
    `zlink_msg_init_size()`로 만든 core message에 복사한다.
  - core에는 `zlink_msg_init_data()` zero-copy API가 있지만, `ffn=NULL`로 쓰면 core message가
    JS `Buffer` 메모리를 borrowed reference로 보관한다.
  - Node public send 계약은 submit 이후 source Buffer나 Message를 애플리케이션이 재사용할 수
    있다는 의미를 유지해야 한다. borrowed zero-copy는 core 송신 큐가 payload를 소비하기 전에
    JS Buffer가 변하거나 해제될 수 있어 public contract를 깨뜨릴 수 있다.
  - JS Buffer lifetime을 native free callback과 묶는 방식도 N-API 객체 참조 해제를 임의 core/IO
    thread에서 수행할 수 있어 안전한 내부 최적화로 보기 어렵다.
  - `zlink_msg_init_buffer()`는 core 내부에서 `init_size()+memcpy()`를 수행하므로 현재
    `init_msg_from_bytes()`와 의미 있는 차이가 없다.
- 판정:
  - zero-copy send는 public contract와 Buffer lifetime 안전성을 건드리는 후보라 구현하지 않는다.
  - `init_buffer` 치환은 같은 복사 경로라 targeted perf 후보로 삼지 않는다.

## multi routed perf runner pending queue 후보 재확인 제외

- 대상:
  - `bindings/node/perf/multi/perf_multi_dealer_router_server.ts`
  - `bindings/node/perf/multi/perf_multi_router_router_server.ts`
  - pending queue의 `Array.shift()` 경로
- 검토:
  - 현재 server runner는 pending reply drain에서 `pending.shift()`를 사용한다.
  - `MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER large head-index queue 후보`는 이미 complete 측정으로
    기각되어 최종 코드와 계획 문서 표에 반영하지 않는다고 기록되어 있다.
  - 이 변경은 bindings library 내부 구현이 아니라 perf runner hot path 변경이다.
- 판정:
  - perf runner는 버그, perf 정책 불일치, C perf 의미 불일치가 명확할 때만 수정한다는 현재
    goal 제약에 따라 다시 적용하지 않는다.
  - multi routed 65536B 개선은 runner queue 우회가 아니라 bindings runtime/native 내부에서만
    추가 후보를 찾는다.

## Received send context direct function 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `Received.send()` 내부 send context 저장/호출 경로
- 근거:
  - `materializeReceived()`가 매 수신마다 `{ send(...) { ... } }` wrapper 객체를 만들고,
    `Received.send()`가 다시 closure를 만들어 `ReceivedSendOperation`에 넘긴다.
  - send context를 내부 함수 값으로 직접 저장하면 wrapper 객체 하나를 줄일 수 있어
    `MULTI_SPOT_SENDSEND`의 routed send/reply hot path에 효과가 있는지 확인했다.
  - public contract에는 노출하지 않고 `bindings/node/dist/index.d.ts` 변경 없이 내부 타입만
    바꾸는 후보로 검증했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_send_context_direct_function_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_043238_node_multi_spot_sendsend_send_context_direct_function_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 20.6/19.8/21.8%
  - `ws 64/256/1024B`: 16.7/18.8/21.4%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## SPOT routed no-wait 수신 경로 후보 적용

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `Spot.recvRouted(..., RecvFlags.DontWait)` no-data 경로
- 근거:
  - 일반 SPOT subscribe는 native `spotRecvNoWait()`가 있어 no-data에서 `null`을 직접 반환한다.
  - routed SPOT 수신은 전용 no-wait native 함수가 없어, drain 루프의 빈 수신마다
    `spotRecvRouted()`가 native error를 만들고 TS catch 경로를 지난다.
  - C perf와 같은 `DontWait` 의미를 유지하면서 예외를 정상적인 no-data 반환으로 바꾸는
    bindings 내부 최적화다. public API와 HWM/profile/runner는 바꾸지 않는다.
- 변경:
  - native addon에 내부 함수 `spotRecvRoutedNoWait`를 추가했다.
  - `Spot.recvRouted()`가 `RecvFlags.DontWait`일 때 이 내부 함수를 사용한다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_routed_nowait_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_043740_node_multi_spot_sendsend_routed_nowait_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 33.5/29.6/36.4%
  - `ws 64/256/1024B`: 23.0/32.1/25.9%
- 판정:
  - `MULTI_SPOT_SENDSEND tcp 64B`, `tcp 1024B`가 새로 통과했다.
  - public surface 변경 없이 통과 항목이 늘었으므로 후보를 유지하고 계획 문서 표에 반영했다.

### SPOT routed no-wait wss/tls 보강 측정

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `wss/tls 64/256/1024B`
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_routed_nowait_wss_tls_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_044109_node_multi_spot_sendsend_routed_nowait_wss_tls_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `wss 64/256/1024B`: 24.3/26.6/28.8%
  - `tls 64/256/1024B`: 25.1/32.0/30.5%
- 판정:
  - 새 통과 항목은 없지만 기존 표의 small 값보다 모두 개선됐다.
  - 후보 자체는 tcp에서 통과 항목을 만들었으므로 유지하고, wss/tls small 최신 complete 값을
    계획 문서 표에 함께 반영했다.

### SPOT routed no-wait reqrep 대용량 보강 측정

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_routed_nowait_large_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_044245_node_multi_spot_reqrep_routed_nowait_large_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 131072B`: 22.8%
  - `ws 131072B`: 21.8%
- 판정:
  - 새 `미달 -> 통과` 항목은 없다.
  - 후보 자체는 `MULTI_SPOT_SENDSEND tcp`에서 통과 항목을 만들었으므로 유지하지만,
    `MULTI_SPOT_REQREP 131072B`에는 통과권 효과가 없었다.

## PUBSUB single-part raw 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - native `socketSubscribeMessage` / `socketTrySubscribeMessage`의 single-part raw 생성 경로
- 근거:
  - PUBSUB perf는 모두 single-part payload를 수신한다.
  - native가 항상 `parts` JS 배열을 만들고 TS materializer가 다시 single-part fast path로
    `Message` 배열을 만들기 때문에, 내부 raw 형태에서 single `part` 필드를 허용하면 native
    배열 생성 비용을 줄일 수 있는지 확인했다.
  - public `TopicMessage.parts` 계약은 그대로 유지하고, native→TS 내부 raw 형태만 바꾸는
    후보로 검증했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
  - build와 병렬로 먼저 실행한 `optimization_guard` 1회는 `dist-tools` 정리 중 파일이 없어 실패했으며,
    build 완료 후 재실행해 통과를 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_single_part_raw_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_045046_node_multi_pubsub_single_part_raw_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 새 `미달 -> 통과` 항목이 없다.
  - `ws 131072B`는 33.9%로 기존 계획 문서의 통과권 값보다 낮아질 수 있다.
  - `tls 65536B`도 32.7%로 기존 통과권 값보다 낮아질 수 있다.
- 판정:
  - 새 통과 항목이 없고 기존 통과권 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## PUBSUB single-part raw 후보 기준 변경 후 재검토 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - 과거 `PUBSUB single-part raw 후보`는 당시 기준에서 새 통과 항목이 없어 기각했다.
  - 현재 Node 단순 one-way 최소 기준은 30%이므로, 같은 complete report 기준에서
    `tls 1024B`가 통과권에 오를 수 있음을 확인하고 재검토했다.
  - public `TopicMessage.parts` 계약은 유지하고, native-to-TS 내부 raw 형태만 single-part일 때
    `parts` 배열 대신 `part` 하나를 허용하는 방식으로 다시 적용했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/native/src/addon_core.cc bindings/node/src/zlink/runtime/messaging/message_materializer.ts`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_single_part_raw_node_threshold_recheck_20260602`
  - Node: `perf_node_multi_linux_20260602_105231_node_multi_pubsub_single_part_raw_node_threshold_recheck_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp`: 20.7/23.4/50.7/14.0/22.0%
  - `ws`: 23.5/22.4/25.9/31.0/32.8%
  - `wss`: 21.7/20.2/33.9/34.4/37.3%
  - `tls`: 22.1/21.6/30.3/30.5/33.7%
- 판정:
  - 기존 미달이던 `MULTI_PUBSUB tls 1024B`는 새로 통과한다.
  - 그러나 기존 accepted PUBSUB report 대비 `tcp 65536B`는 18.4%에서 14.0%,
    `tcp 131072B`는 26.9%에서 22.0%로 더 낮아졌다.
  - 기존 통과였던 large cell도 `ws 65536/131072B`, `wss 65536B`, `tls 65536/131072B`가
    통과 상태는 유지하지만 수치가 내려갔다.
  - 목표 원칙상 통과 증가가 있더라도 회귀가 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT recvRouted RoutingId 재사용 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `Spot.recvRouted()`의 source/spot routing id materialize와 send/reply closure 경로
- 근거:
  - `recvRouted()`는 수신 envelope용 `RoutingId`를 만들고, 이후 `Received.send()` 또는
    `reply()` closure에서 같은 raw buffer를 `RoutingId.from(...)`으로 다시 복사한다.
  - 수신 시점에 `RoutingId.fromOwnedBuffer(...)`로 만든 객체를 envelope와 closure가 함께 쓰면
    public API를 바꾸지 않고 routed SPOT echo hot path의 routing id 복사를 줄일 수 있는지 확인했다.
  - closure는 mutable `Received`가 아니라 수신 시점의 `RoutingId` 객체를 캡처하도록 해,
    operation이 기존처럼 수신 시점의 대상 route에 묶이는 의미를 유지했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_routingid_reuse_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_050145_node_multi_spot_sendsend_routingid_reuse_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 새 `미달 -> 통과` 항목이 없다.
  - 직전 accepted 값에서 통과였던 `MULTI_SPOT_SENDSEND tcp 64B`와 `tcp 1024B`가 각각
    26.5%, 31.6%로 미달권에 내려갈 수 있다.
  - `wss 65536/131072B`도 통과권은 유지하지만 기존 accepted 값보다 크게 낮다.
- 판정:
  - 새 통과 항목이 없고 기존 통과권 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## reply flags fast path 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - Router/Spot reply 내부의 기본 `SendFlags.None` 검증 경로
- 근거:
  - perf reply hot path는 `flags()`를 호출하지 않아 `SendFlags.None`으로 submit된다.
  - `normalizeReplyFlags()`는 non-zero flags일 때만 public 오류 의미가 필요하므로,
    기본 flags에서는 함수 호출을 건너뛰면 reply submit 비용을 줄일 수 있는지 확인했다.
  - non-zero flags는 기존처럼 `SubmitResult.NotSupported`를 던지도록 유지했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_reply_flags_fastpath_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_050455_node_multi_spot_reqrep_reply_flags_fastpath_probe_20260602.txt`
  - status: partial
- 결과:
  - `MULTI_SPOT_REQREP tcp 131072B` run 2가 client failure로 실패했고, report는
    `status=partial`이었다.
- 판정:
  - partial report는 계획 문서 표 근거로 사용할 수 없다.
  - 첫 targeted 조건에서 실패가 발생했으므로 안정성 회귀 후보로 보고 최종 코드와 계획 문서
    표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB subscribed raw 객체 define_properties 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - native `create_subscribed_value()` raw 객체 생성 경로
- 근거:
  - `create_subscribed_value()`는 수신마다 `routingId`, `topic`, `parts` 세 property를
    `napi_set_named_property()`로 순차 설정한다.
  - public `TopicMessage` 계약과 perf runner를 그대로 두고, native 내부 raw 객체 property
    정의만 `napi_define_properties()`로 묶으면 PUBSUB small hot path의 N-API 호출 비용을
    줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_define_properties_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_051723_node_multi_pubsub_define_properties_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 새 `미달 -> 통과` 항목이 없다.
  - C 대비 비율은 `tcp 64/256/1024/65536/131072B`가 19.2/21.5/47.4/14.9/21.2%,
    `ws`가 22.4/22.0/26.5/31.8/33.8%, `wss`가 20.2/18.7/32.2/38.1/39.3%,
    `tls`가 20.6/17.7/28.2/34.1/35.7%였다.
  - 기존 계획 문서의 accepted 값보다 낮아질 수 있는 셀이 많다.
- 판정:
  - 통과 항목이 늘지 않고 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## single SPOT recv topic stack buffer 후보 기각

- 대상:
  - single `SPOT`
  - `tcp 65536/131072/262144B`
  - native `spot_recv()` / `spot_try_recv()` topic buffer 준비 경로
- 근거:
  - SPOT 수신 hot path는 수신마다 `std::vector<char>(256)`으로 topic buffer를 만든다.
  - perf topic은 256B 이하라 stack buffer로 충분하므로, topic이 더 길 때만 heap vector로
    fallback하면 public contract와 perf runner 의미를 바꾸지 않고 수신 비용을 줄일 수 있는지
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern SPOT --msg-sizes 65536,131072,262144 --duration 1 --runs 3 --results-tag node_single_spot_recv_topic_stack_probe_20260602`
  - Node: `perf_node_single_linux_20260602_051935_node_single_spot_recv_topic_stack_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - C 대비 `tcp 65536/131072/262144B`: 35.9/30.2/28.1%
  - `131072B`는 기존 계획 문서의 28.9%보다 조금 올랐지만 여전히 미달이다.
  - `262144B`와 기존 통과권인 `65536B`는 기존 계획 문서 값보다 낮아질 수 있다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없고 기존 통과/대용량 값 회귀 위험이 있으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## message snapshot data property key cache 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `create_message_snapshot_value()`의 `data` property 설정 경로
- 근거:
  - 모든 receive snapshot은 native에서 `{ data: Buffer }` property를 만든 뒤 TypeScript
    materializer가 `Message`로 감싼다.
  - `napi_define_properties()` 후보는 이미 기각됐지만, snapshot shape는 그대로 유지하면서
    `"data"` property key만 `napi_ref`로 캐시해 `napi_set_property()`에 넘기면 매 수신 part의
    property name 처리 비용을 줄일 수 있는지 확인했다.
  - public `Message`/`Received` 계약과 `.d.ts` surface는 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_data_key_cache_probe_20260602`
  - Node: `perf_node_single_linux_20260602_052616_node_single_routed_data_key_cache_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 64/256/1024B`: 19.1/19.8/22.3%
  - `DEALER_ROUTER ws 64/256/1024B`: 19.7/23.2/30.8%
  - `DEALER_ROUTER wss 64/256/1024B`: 19.5/22.2/49.0%
  - `DEALER_ROUTER tls 64/256/1024B`: 20.1/20.8/32.9%
  - `ROUTER_ROUTER tcp 64/256/1024B`: 22.6/23.1/23.7%
  - `ROUTER_ROUTER ws 64/256/1024B`: 22.2/23.9/30.6%
  - `ROUTER_ROUTER wss 64/256/1024B`: 22.1/23.5/50.5%
  - `ROUTER_ROUTER tls 64/256/1024B`: 21.5/22.5/33.0%
- 판정:
  - 핵심 미달인 64/256B와 tcp 1024B는 기준에 못 닿았다.
  - `wss 1024B`는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB topic string single-entry cache 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - native `create_subscribed_value()`의 topic JS string 생성 경로
- 근거:
  - PUBSUB perf는 같은 topic을 반복 수신하지만 native는 매 수신마다 새 JS string을 만든다.
  - 이전 검토에서는 수명/해제 정책 없는 persistent cache라 후보에서 제외했으므로, 이번에는
    env별 single-entry cache와 cleanup hook을 붙인 bounded cache 형태로만 검증했다.
  - public `TopicMessage.topic`은 immutable string 값이므로 public `.d.ts`와 계약은 바꾸지 않는다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_topic_string_cache_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_053444_node_multi_pubsub_topic_string_cache_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - C 대비 비율은 `tcp 64/256/1024/65536/131072B`가 20.4/22.8/49.8/14.8/21.3%,
    `ws`가 23.5/22.2/25.9/31.3/33.9%, `wss`가 21.2/20.1/33.6/38.1/40.5%,
    `tls`가 22.0/20.7/28.9/31.7/34.6%였다.
  - `wss 65536/131072B`는 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 기존 계획 문서에서 통과였던 `tls 131072B`는 미달권으로 내려갈 수 있다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없고 기존 통과권 회귀 위험이 있으므로 최종 코드와 계획 문서
    표에는 반영하지 않고 되돌렸다.

## routed message properties single-entry cache 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `create_message_properties_snapshot()`의 synthetic `Routing-Id`/`Identity` properties 생성 경로
- 근거:
  - routed receive는 같은 peer routing id를 반복 수신하므로, 동일 routing id의 properties 객체를
    env별 single-entry `napi_ref`로 재사용하면 small routed path의 JS 객체 생성 비용을 줄일 수
    있는지 확인했다.
  - public `MessageSnapshot.properties` shape와 `.d.ts` surface는 바꾸지 않았다.
  - cache에는 env cleanup hook을 붙여 수명 누수 없이 bounded cache로만 검증했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_properties_cache_probe_20260602`
  - Node: `perf_node_single_linux_20260602_054129_node_single_routed_properties_cache_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 64/256/1024B`: 22.0/20.8/24.2%
  - `DEALER_ROUTER ws 64/256/1024B`: 21.2/23.9/32.9%
  - `DEALER_ROUTER wss 64/256/1024B`: 21.1/23.6/53.3%
  - `DEALER_ROUTER tls 64/256/1024B`: 21.9/22.7/35.0%
  - `ROUTER_ROUTER tcp 64/256/1024B`: 23.5/24.5/25.2%
  - `ROUTER_ROUTER ws 64/256/1024B`: 23.8/24.9/33.1%
  - `ROUTER_ROUTER wss 64/256/1024B`: 23.9/24.5/52.7%
  - `ROUTER_ROUTER tls 64/256/1024B`: 23.0/23.7/34.4%
- 판정:
  - 핵심 미달인 64/256B와 tcp 1024B는 기준에 못 닿았다.
  - 일부 1024B 셀은 통과지만 기존 계획 문서에서도 이미 통과였던 셀이다.
  - 새 `미달 -> 통과` 항목이 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## routed message properties `napi_define_properties()` 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `create_message_properties_snapshot()`의 synthetic `Routing-Id`/`Identity` property 설정 경로
- 근거:
  - properties 객체에 같은 JS string 값을 `Routing-Id`와 `Identity`로 두 번 설정한다.
  - `napi_set_named_property()` 두 번을 `napi_define_properties()` 한 번으로 줄이면 native property
    설정 비용이 낮아지는지 검증했다.
  - public `MessageSnapshot.properties` shape와 `.d.ts` surface는 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_properties_define_probe_20260602`
  - Node: `perf_node_single_linux_20260602_054652_node_single_routed_properties_define_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 64/256/1024B`: 19.0/19.4/22.1%
  - `DEALER_ROUTER ws 64/256/1024B`: 19.9/22.1/30.7%
  - `DEALER_ROUTER wss 64/256/1024B`: 19.9/22.7/49.5%
  - `DEALER_ROUTER tls 64/256/1024B`: 19.9/21.0/32.8%
  - `ROUTER_ROUTER tcp 64/256/1024B`: 21.9/23.0/24.0%
  - `ROUTER_ROUTER ws 64/256/1024B`: 22.5/23.4/30.3%
  - `ROUTER_ROUTER wss 64/256/1024B`: 21.9/23.0/49.6%
  - `ROUTER_ROUTER tls 64/256/1024B`: 21.4/22.5/32.6%
- 판정:
  - `napi_define_properties()` 적용 후 통과 셀이 5개에서 2개로 줄었다.
  - 기존 통과권인 `DEALER_ROUTER tls 1024B`, `ROUTER_ROUTER tls 1024B`,
    `ROUTER_ROUTER ws 1024B`가 미달권으로 내려가는 회귀 위험이 있다.
  - 새 `미달 -> 통과` 항목이 없고 회귀가 명확하므로 최종 코드와 계획 문서 표에는 반영하지
    않고 되돌렸다.

## MULTI_STREAM packet routing id last-entry cache 후보 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `StreamSocket.packetRoutingId()`의 packet callback routing id wrapping 경로
- 근거:
  - packet handler는 native callback마다 routing id `Buffer`를 `latin1` string key로 변환한 뒤
    `Map`에서 `RoutingId` wrapper를 찾는다.
  - 같은 peer에서 연속 packet이 들어오는 경우 직전 routing id를 byte 비교로 재사용하면 string
    key 생성 비용을 줄일 수 있는지 확인했다.
  - public `StreamPacketHandler` 계약과 `.d.ts` surface는 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_packet_rid_last_cache_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_055144_node_multi_stream_packet_rid_last_cache_probe_20260602.txt`
  - status: partial
- 결과:
  - `tcp 64/256/1024B`는 95.877/91.409/87.297 Kops/s로 측정됐다.
  - `ws 64/256B`는 57.845/56.004 Kops/s로 측정됐다.
  - `ws 1024B`에서 `malloc(): unaligned tcache chunk detected` 이후 client failure가 발생했다.
  - report completion은 `success=5`, `fail=1`, `status=partial`이었다.
- 판정:
  - partial report이므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - routing id `Buffer`를 callback 이후 객체 상태에 붙잡는 형태는 native callback buffer 수명과
    충돌할 수 있는 안정성 위험이 있다.
  - 새 `미달 -> 통과` 증거가 없고 불안정 failure가 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## MULTI_STREAM packet routing id owned last-entry cache 후보 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `StreamSocket.packetRoutingId()`의 packet callback routing id wrapping 경로
- 근거:
  - 직전 후보가 callback `Buffer`를 보관해 안정성 위험이 있었으므로, 이번에는
    `Buffer.from(routingId)`로 소유 복사본만 보관하는 bounded last-entry cache로 검증했다.
  - hit일 때만 `latin1` string key 생성을 피하고, public `StreamPacketHandler` 계약과 `.d.ts`
    surface는 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_packet_rid_owned_last_cache_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_055425_node_multi_stream_packet_rid_owned_last_cache_probe_20260602.txt`
  - status: partial
- 결과:
  - `tcp 64B`는 102.361 Kops/s로 측정됐다.
  - `tcp 256B`에서 `malloc(): unaligned tcache chunk detected` 이후 client failure가 발생했다.
  - report completion은 `success=2`, `fail=1`, `status=partial`이었다.
- 판정:
  - 소유 복사본 변형에서도 native/client failure가 재발했으므로 packet routing id last-entry
    cache 계열은 안정성 위험이 있다.
  - partial report이므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - 새 `미달 -> 통과` 증거가 없고 불안정 failure가 있으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## MULTI_SPOT_SENDSEND sendToSpotDirect closure 제거 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `Spot.sendToSpotDirect()`의 `submitSpotSend()` closure 생성 경로
- 근거:
  - `MULTI_SPOT_SENDSEND` client와 server echo는 `sendToSpotDirect()`를 반복 호출한다.
  - 기존 구현은 매 호출마다 `submitSpotSend()`에 넘기는 closure를 만든다.
  - public `Spot.sendToSpot()` 계약과 native binding surface는 바꾸지 않고, 해당 method 내부를
    직접 `try/catch` 형태로 펼쳐 closure 생성 비용을 줄일 수 있는지 검증했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_send_direct_no_closure_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_060129_node_multi_spot_sendsend_send_direct_no_closure_probe_20260602.txt`
  - status: partial
- 결과:
  - `tcp 64/256/1024B`: 78.686/61.178/70.623 Kops/s
  - `ws 64/256/1024B`: 79.118/75.735/64.555 Kops/s
  - `wss 64/256/1024B`: 57.363/57.928/60.205 Kops/s
  - `tls 64/1024B`: 61.901/61.354 Kops/s
  - `tls 256B`에서 `spot_node_set_pub_bind failed: Address already in use`로 client ready 전에 실패했다.
  - report completion은 `success=11`, `fail=1`, `status=partial`이었다.
- 판정:
  - complete report가 아니므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - 일부 중간 수치는 개선 가능성을 보였지만, perf 원칙상 partial 결과로 통과 판정을 만들 수 없다.
  - 새 `미달 -> 통과`의 complete 증거가 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## MULTI routed large `singlePartOrThrow()` 직접 길이 검사 후보 기각

- 대상:
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536/131072B`
  - `MultipartEnvelope.singlePartOrThrow()`의 hot path 검사 경로
- 근거:
  - routed echo server는 수신 payload를 `received.singlePartOrThrow()`로 꺼내 즉시 reply한다.
  - 기존 구현은 `singlePartOrThrow()` 내부에서 `isSinglePart()` method를 한 번 더 호출한다.
  - public method shape, 예외 타입, 예외 메시지는 유지하면서 내부 검사를 `this.parts.length !== 1`
    직접 검사로 바꾸면 large routed echo path의 작은 JS 호출 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_large_single_part_direct_len_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_060747_node_multi_routed_large_single_part_direct_len_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`: 19.1/24.3%
  - `MULTI_DEALER_ROUTER ws 65536/131072B`: 21.1/29.9%
  - `MULTI_ROUTER_ROUTER tcp 65536/131072B`: 24.7/30.4%
  - `MULTI_ROUTER_ROUTER ws 65536/131072B`: 25.7/31.5%
- 판정:
  - 핵심 미달인 `65536B` 셀은 모두 기준에 못 닿았다.
  - 기존 계획 문서에서 통과였던 `MULTI_DEALER_ROUTER tcp/ws 131072B`가 미달권으로 내려갈 수 있다.
  - 새 `미달 -> 통과` 항목이 없고 기존 통과권 회귀 위험이 있으므로 최종 코드와 계획 문서
    표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB publish operation closure 제거 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
  - `PublisherSocket.publish()`가 hot path에서 만드는 arrow closure
- 근거:
  - `PublisherSocket.publish()`는 매 호출마다 `this.publishDirect()`를 감싼 closure를 만들어
    `PublishOperation`에 넘긴다.
  - public `SendOperation` 계약과 `dist/index.d.ts`를 바꾸지 않고, `PublishOperation` 내부에
    선택적 receiver를 보관해서 closure 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_publish_operation_no_closure_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_061722_node_multi_pubsub_publish_operation_no_closure_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - 통과 2개, 미달 18개였다.
  - `MULTI_PUBSUB tcp 1024B`: 49.2%
  - `MULTI_PUBSUB wss 131072B`: 37.2%
  - 직전 채택값 `perf_node_multi_linux_20260601_232020_node_multi_pubsub_topic_replace_no_close_probe_20260601.txt`는
    통과 6개, 미달 14개였다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없고, 기존 채택값보다 통과 항목이 줄었다.
  - receiver 분기와 `Function.call()` 비용이 closure 제거 이득보다 커질 수 있어 hot path에 유지할
    근거가 없다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_STREAM send operation routing id 캐시 후보 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `StreamSocket.send(routingId)`의 public send operation 내부 경로
- 근거:
  - `StreamSocket.send()`는 `RuntimeSendOperation`에 closure를 넘기고, submit 시점에 다시
    `normalizeRoutingId(routingId)`를 수행한다.
  - public `StreamSocket.send()` 계약과 `.d.ts`는 유지하면서 내부 operation이 생성 시점의
    routing id Buffer를 보관하면 stream echo hot path의 closure와 routing id 정규화 비용을
    줄일 수 있는지 확인했다.
  - perf runner의 의미나 C 비교 조건은 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_send_operation_cached_rid_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_062507_node_multi_stream_send_operation_cached_rid_probe_20260602.txt`
  - C full 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - C tcp 제한 기준: `perf_c_multi_linux_20260601_142813_node_multi_stream_tcp_small_c_recheck_20260601.txt`
  - status: complete
- 결과:
  - C full 기준 통과 0개, 미달 12개였다.
  - `tcp 64/256/1024B`: 27.2/28.0/26.5%
  - `ws 64/256/1024B`: 19.6/20.5/20.0%
  - `wss 64/256/1024B`: 28.0/28.8/25.9%
  - `tls 64/256/1024B`: 25.6/26.6/25.8%
  - C tcp 제한 기준에서도 `tcp 64/256/1024B`는 28.7/31.6/27.2%로 모두 미달이다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - operation 내부 중복 제거만으로는 stream small의 C 대비 차이를 줄이지 못했다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single SPOT TopicMessage storage 재사용 후보 기각

- 대상:
  - single `SPOT`
  - `tcp 131072/262144B`
  - `perf_spot.ts`의 `spot.subscribe(result, flags)` caller-provided storage 사용 방식
- 근거:
  - `Spot.subscribe(result, flags)`는 caller-provided storage API인데, single SPOT runner는
    매 수신마다 `new TopicMessage()`를 만들고 있었다.
  - C single SPOT은 receive storage를 반복 재사용하므로, 이 후보는 목표치/HWM/profile 변경이
    아니라 C perf 의미와 맞추는 perf-runner 예외 후보로만 검토했다.
  - binding public contract와 library 구현은 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern SPOT --msg-sizes 131072,262144 --duration 1 --runs 3 --results-tag node_single_spot_reuse_topic_storage_tcp_large_probe_20260602`
  - Node: `perf_node_single_linux_20260602_062810_node_single_spot_reuse_topic_storage_tcp_large_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `SPOT tcp 131072B`: 28.3%
  - `SPOT tcp 262144B`: 29.8%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서의 28.9/31.0%보다 낮아졌으므로 runner 변경을 유지할 근거도 없다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single SPOT publish topic prevalidation 후보 채택

- 대상:
  - single `SPOT`
  - `tcp 131072/262144B`
  - `Spot.publish(topic)` hot path의 반복 topic 검증
- 근거:
  - `Spot.publish(topic)`은 public send operation을 만들지만, 기존 구현은 submit 때마다
    `validateCString(topic, 'topic', Number.MAX_SAFE_INTEGER)`를 다시 수행했다.
  - `publish(topic)` 호출 시점에 topic을 한 번 검증해 operation closure가 검증된 string을
    보관하면 public contract와 `.d.ts` 변경 없이 submit hot path의 문자열 scan/byte length
    계산을 줄일 수 있다.
  - 이는 HWM/profile/목표치/C baseline을 바꾸지 않는 bindings library 내부 개선이다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp --pattern SPOT --msg-sizes 131072,262144 --duration 1 --runs 3 --results-tag node_single_spot_publish_topic_prevalidate_tcp_large_probe_20260602`
  - Node: `perf_node_single_linux_20260602_063157_node_single_spot_publish_topic_prevalidate_tcp_large_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `SPOT tcp 131072B`: 34.3%, 통과
  - `SPOT tcp 262144B`: 35.7%, 통과
- 판정:
  - 두 셀이 새 `미달 -> 통과` 항목이므로 후보를 유지한다.
  - 계획 문서의 Node single `SPOT tcp 131072/262144B` 셀을 이 complete report 기준으로 갱신했다.

## MULTI_SPOT_SENDSEND sendToSpot routing id pre-normalization 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `Spot.sendToSpot(destNodeRid, destSpotRid)` public send operation의 routing id 정규화 경로
- 근거:
  - `MULTI_SPOT_SENDSEND` client는 active send마다
    `spot.sendToSpot(SERVER_NODE_ROUTING_ID, SERVER_SPOT_ROUTING_ID).message(...).submit()`을 호출한다.
  - 기존 구현은 submit 시점마다 `normalizeRoutingId(destNodeRid)`와
    `normalizeRoutingId(destSpotRid)`를 다시 수행한다.
  - public API와 `.d.ts`는 유지하고 operation 생성 시점에 routing id Buffer를 보관하면
    client send hot path의 반복 정규화 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_send_to_spot_pre_normalize_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_063618_node_multi_spot_sendsend_send_to_spot_pre_normalize_probe_20260602.txt`
  - status: partial
- 결과:
  - `tcp 64/256B`는 92.494/90.302 Kops/s로 높게 나왔다.
  - `tcp 1024B`에서 client timeout이 발생했고, report completion은 `success=2`, `fail=1`,
    `status=partial`이었다.
- 판정:
  - complete report가 아니므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - `MULTI_SPOT_SENDSEND`는 timeout/partial 이력이 있는 패턴이고, 이번 후보도 fail-fast
    timeout을 만들었으므로 안정성 위험이 있다.
  - 새 `미달 -> 통과`의 complete 증거가 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## MULTI_SPOT_REQREP callback request fast path 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `ws 131072B`
  - `Spot.requestToSpot(...).submit(callback)`의 callback request submit 경로
- 근거:
  - `MULTI_SPOT_REQREP` client는 active request마다
    `spot.requestToSpot(...).message(...).flags(DontWait).timeout(...).submit(callback)`을 호출한다.
  - 기존 `executeSpotRequest()`는 callback mode에서도 `executeNativeRequest({ ... })` 옵션 객체와
    중간 invoke closure를 매 요청마다 만든다.
  - public `RequestOperation` 계약과 `.d.ts`는 유지하고, callback mode만 `executeSpotRequest()`
    내부에서 직접 `startRequestProgress()`와 native `spotRequestSpot` invoker를 호출하면
    callback submit hot path의 중간 객체/closure 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_callback_fastpath_ws_large_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_064159_node_multi_spot_reqrep_callback_fastpath_ws_large_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP ws 131072B`: Node 5.551 Kops/s, C 25.028 Kops/s, 22.2%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - callback submit의 TypeScript 중간 객체/closure 제거만으로는 `ws 131072B`의 C 대비 차이를
    줄이지 못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## Runtime operation payload identity 함수 재사용 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`, `MULTI_SPOT_SENDSEND`
  - `tcp/ws 64/256/1024B`
  - `RuntimeSendOperation`, `PublishOperation`, `RuntimeRequestOperation`, `RuntimeReplyOperation`
    내부 `OperationPayload` normalize 함수 생성 경로
- 근거:
  - runtime operation builder는 operation 객체를 만들 때마다 `new OperationPayload(..., (message) => message)`
    형태의 identity lambda를 새로 만든다.
  - 이전 `Runtime operation builder payload 상태 흡수 후보`는 payload 상태 구조 자체를 바꾸는 큰
    후보였고 complete 측정에서 기각됐다. 이번 후보는 상태 구조와 public builder 계약을 유지한 채
    identity normalize 함수만 module-level 함수로 공유해 per-operation 함수 allocation만 줄이는
    더 좁은 내부 개선으로 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_PUBSUB,MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_operation_payload_identity_function_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_064913_node_multi_operation_payload_identity_function_probe_20260602.txt`
  - status: partial
- 결과:
  - `MULTI_PUBSUB tcp 64/256/1024B`: 548.137/548.378/503.137 Kmsg/s
  - `MULTI_PUBSUB ws 64/256/1024B`: 509.699/537.300/524.051 Kmsg/s
  - `MULTI_SPOT_SENDSEND tcp 256/1024B`: 72.524/66.482 Kops/s
  - `MULTI_SPOT_SENDSEND tcp 64B`: client timeout after 90000ms
  - report completion은 `success=8`, `fail=1`, `status=partial`이었다.
- 판정:
  - complete report가 아니므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - `MULTI_SPOT_SENDSEND`는 timeout/partial 이력이 있는 패턴이고, 이번 후보도 fail-fast
    timeout을 만들었으므로 안정성 위험이 있다.
  - 새 `미달 -> 통과`의 complete 증거가 없으므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## Native blocking send 반환 byte-count 제거 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER` `tcp/ws 64/256/1024B`
  - multi `MULTI_PUBSUB`, `MULTI_SPOT_SENDSEND` `tcp/ws 64/256/1024B`
  - native `socketSend`, `socketSendParts`, `socketPublish`, `socketSendRouting`의 blocking send 반환 경로
- 근거:
  - TypeScript runtime은 blocking `socketSend`, `socketSendParts`, `socketPublish`, `socketSendRouting`
    반환값을 사용하지 않는다.
  - native 함수가 성공 후 `zlink_msg_size()` 합산과 `napi_create_int32()`를 수행하므로, 이 값을
    만들지 않으면 blocking fallback/publish/routed send 경로의 작은 비용을 줄일 수 있는지 확인했다.
  - public `.d.ts` 계약은 변경하지 않고 runtime-owned native binding 내부 선언만 함께 맞추는 후보였다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - single 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_send_return_void_routed_small_probe_20260602`
  - single Node: `perf_node_single_linux_20260602_065637_node_single_send_return_void_routed_small_probe_20260602.txt`
  - single C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - single status: complete
  - multi 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_PUBSUB,MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_send_return_void_pubsub_spot_probe_20260602`
  - multi Node: `perf_node_multi_linux_20260602_070104_node_multi_send_return_void_pubsub_spot_probe_20260602.txt`
  - multi C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - multi status: complete
- 결과:
  - single routed small 비교: 통과 0개, 미달 12개
  - multi 비교: 통과 2개, 미달 10개
  - multi 통과 항목은 기존 표에서도 이미 통과였던 `MULTI_PUBSUB tcp 1024B`,
    `MULTI_SPOT_SENDSEND tcp 1024B`뿐이었다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 반환 byte-count 생성 비용 제거만으로는 Node routed/publish/SPOT_SENDSEND 병목을 의미 있게 줄이지
    못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## STREAM packet callback 배열 제거 후보 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
  - native `socketStreamAttach`의 runtime-owned packet callback 전달 경로
- 근거:
  - public `StreamPacketHandler` 계약은 `(sourceRid, header, body)`인데, native 내부 callback은
    `routingId`와 `Buffer[]`를 TypeScript wrapper에 전달한 뒤 wrapper가 `packets[0]`, `packets[1]`을
    다시 꺼낸다.
  - public `.d.ts` 계약을 바꾸지 않고 runtime-owned native callback만
    `(routingId, headerBuffer, bodyBuffer)`로 바꾸면 packet마다 JS 배열 할당과 element set 비용을
    줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_packet_callback_direct_args_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_070748_node_multi_stream_packet_callback_direct_args_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_STREAM tcp 64/256/1024B`: 27.5/27.3/28.1%
  - `MULTI_STREAM ws 64/256/1024B`: 19.3/20.4/19.7%
  - `MULTI_STREAM wss 64/256/1024B`: 28.2/27.9/27.2%
  - `MULTI_STREAM tls 64/256/1024B`: 25.2/26.6/25.5%
  - 비교 결과 통과 0개, 미달 12개였다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - packet 배열 할당 제거만으로는 `MULTI_STREAM`의 C 대비 차이를 줄이지 못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## RoutingId.fromOwnedBuffer 중복 검증 제거 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - routed receive materialize의 `RoutingId.fromOwnedBuffer()` 경로
- 근거:
  - `wrapNativeRoutingId()`는 native가 넘긴 `Buffer`에서 null/empty를 먼저 거른 뒤
    `RoutingId.fromOwnedBuffer()`를 호출한다.
  - `fromOwnedBuffer()`의 `Buffer.isBuffer()`와 길이 검사는 native receive materialize hot path에서는
    중복일 수 있으므로, public `.d.ts` 계약을 바꾸지 않고 internal 함수의 중복 검사를 제거해
    routing id wrapper 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_owned_rid_unchecked_probe_20260602`
  - Node: `perf_node_single_linux_20260602_071315_node_single_routed_owned_rid_unchecked_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - 비교 결과 통과 3개, 미달 21개였다.
  - 통과 3개는 기존 계획 문서에서도 이미 통과였던 `DEALER_ROUTER tls 1024B`,
    `DEALER_ROUTER wss 1024B`, `ROUTER_ROUTER wss 1024B`뿐이었다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - internal 검증 제거만으로는 routed small의 C 대비 차이를 줄이지 못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## routed large Message payload Buffer normalization 후보 기각

- 대상:
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536/131072B`
  - `normalizeMessageLikePayload()` / `normalizeOperationPayload()`의 `Message` 입력 정규화 경로
- 근거:
  - 기존 `Message send normalization payload Buffer 후보`는 `MULTI_SPOT_SENDSEND` 기준으로
    측정되어 통과 항목을 만들지 못하고 기각됐다.
  - routed echo large server도 수신 `Message`를 즉시 send payload로 되돌리므로, 같은 내부
    후보가 `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER`의 `65536B` 미달 셀에는 효과가 있는지
    별도로 확인했다.
  - public `.d.ts` 계약은 바꾸지 않고, `Message`를 native에 넘길 때 snapshot 객체 대신
    `payloadBuffer()`를 넘기는 내부 변경만 적용했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_message_payload_buffer_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_072035_node_multi_routed_message_payload_buffer_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`: 19.1/25.6%
  - `MULTI_DEALER_ROUTER ws 65536/131072B`: 21.2/29.8%
  - `MULTI_ROUTER_ROUTER tcp 65536/131072B`: 23.9/29.4%
  - `MULTI_ROUTER_ROUTER ws 65536/131072B`: 25.1/32.3%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과였던 `MULTI_DEALER_ROUTER tcp/ws 131072B`와
    `MULTI_ROUTER_ROUTER tcp 131072B`가 미달권으로 내려갈 수 있어 회귀 위험이 있다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## RoutedMessageSocket send routing id pre-normalization 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `RoutedMessageSocket.send(routingId)` public send operation의 routing id 정규화 경로
- 근거:
  - perf runner는 `send(routingId).message(...).submit()` 형태로 active send마다 operation을 만든다.
  - 기존 구현은 submit 시점에 `normalizeRoutingId(routingId)`를 수행한다.
  - public `.d.ts` 계약을 바꾸지 않고 operation 생성 시점에 routing id Buffer를 보관하면
    routed send hot path의 반복 정규화 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_send_prenormalize_probe_20260602`
  - Node: `perf_node_single_linux_20260602_072806_node_single_routed_send_prenormalize_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - 비교 결과 통과 2개, 미달 22개였다.
  - 통과 2개는 기존 계획 문서에서도 이미 통과였던 `DEALER_ROUTER wss 1024B`,
    `ROUTER_ROUTER wss 1024B`뿐이었다.
  - 기존 계획 문서에서 통과였던 `DEALER_ROUTER ws/tls 1024B`,
    `ROUTER_ROUTER ws/tls 1024B`가 이번 측정에서는 미달권으로 내려가 회귀 위험이 있다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - routing id 정규화 시점 이동만으로는 routed small의 C 대비 차이를 줄이지 못했다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT sendToSpot DontWait result-code native 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp 64/256/1024B`
  - `Spot.sendToSpotDirect(..., DontWait)`의 native submit 결과 처리 경로
- 근거:
  - C multi SPOT_SENDSEND는 `zlink_spot_send_spot_part()`의 `zlink_submit_result_t`를 직접 받아
    backpressure를 제어 흐름으로 처리한다.
  - Node는 같은 `DontWait` submit 실패를 native exception으로 던진 뒤 TypeScript에서 `SubmitError`를
    다시 `false`로 바꾼다.
  - public `.d.ts` 계약은 바꾸지 않고 runtime-owned native method만 추가해 `DontWait`에서 result
    code를 직접 받으면 exception 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_nowait_result_tcp_small_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_073312_node_multi_spot_sendsend_nowait_result_tcp_small_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_SENDSEND tcp 64B`: 35.3%, 통과
  - `MULTI_SPOT_SENDSEND tcp 256B`: 31.6%, 미달
  - `MULTI_SPOT_SENDSEND tcp 1024B`: 28.6%, 미달
- 판정:
  - `tcp 64B`는 기존 계획 문서에서도 이미 통과였으므로 새 `미달 -> 통과` 항목이 아니다.
  - `tcp 256B`는 기존 29.6%보다 개선됐지만 SPOT 기준 33%에 못 닿았다.
  - 기존 계획 문서에서 통과였던 `tcp 1024B`가 이번 측정에서는 28.6%로 미달권에 내려가 회귀 위험이 있다.
  - goal 기준상 통과 항목이 늘지 않거나 회귀 위험이 있는 후보는 유지하지 않으므로 최종 코드와
    계획 문서 표에는 반영하지 않고 되돌렸다.

## SPOT sendToSpot DontWait result-code native 후보 기준 변경 후 채택

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
- 근거:
  - 과거 같은 후보는 SPOT 기준을 33%로 보고 기각했다.
  - 현재 Node 기준은 5% 낮아진 `node` 그룹이며, SPOT 최소 기준은 28%다.
  - C multi SPOT_SENDSEND는 `zlink_spot_send_spot_part()`의 `zlink_submit_result_t`를 직접
    받아 backpressure를 제어 흐름으로 처리한다.
  - Node 기존 경로는 `DontWait` submit 실패를 native exception으로 던진 뒤 TypeScript에서
    `SubmitError`를 다시 `false`로 바꾼다.
  - public `.d.ts` 계약은 바꾸지 않고 runtime-owned native method만 추가해 `DontWait`에서
    result code를 직접 받도록 했다.
- 변경:
  - native에 `spotSendToSpotNoWaitResult`를 추가해 `ZLINK_SEND_FLAGS_DONTWAIT` submit result를
    숫자로 반환한다.
  - `Spot.sendToSpotDirect()`는 `SendFlags.DontWait`일 때 이 result-code path를 사용하고,
    public `SendOperation.submit()` 의미는 그대로 `true`/`false`/`SubmitError`로 유지한다.
  - perf runner, HWM/profile, 목표치, C baseline은 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/native/src/addon_spot.cc bindings/node/native/src/addon_spot_api.h bindings/node/native/src/addon_exports.cc bindings/node/src/zlink/runtime/native/binding_service.ts bindings/node/src/zlink/runtime/service/spot/spot.ts doc/plan/perf/log/2026-06-01-node-bindings-performance-round.ko.md`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_nowait_result_small_recheck_20260602`
  - Node: `perf_node_multi_linux_20260602_104143_node_multi_spot_sendsend_nowait_result_small_recheck_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 34.1/31.7/29.0%, 모두 통과
  - `ws 64/256/1024B`: 30.3/29.3/32.6%, 모두 통과
  - `wss 64/256/1024B`: 29.1/23.1/29.8%, 64/1024B 통과, 256B 미달
  - `tls 64/256/1024B`: 28.8/33.5/32.2%, 모두 통과
- 판정:
  - 기존 계획 문서에서 미달이던 `ws 64B`, `ws 1024B`, `wss 64B`, `tls 64B`가 새로 통과했다.
  - 기존 통과였던 small cell은 통과 상태를 유지한다.
  - Node multi 미달은 `33/156 (21.2%)`에서 `29/156 (18.6%)`로 줄었다.
  - public contract와 perf 기준/HWM/profile은 바꾸지 않았고 complete report에서 새 통과가
    확인됐으므로 후보를 유지한다.

## multi routed server Received 재사용 후보 기각

- 대상:
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws 65536/131072B`
  - Node multi routed echo server의 caller-provided `Received` storage 사용 방식
- 근거:
  - `MessageSocket.recv(result, flags)`는 caller-provided storage를 받는 public API다.
  - `MULTI_DEALER_ROUTER`와 `MULTI_ROUTER_ROUTER` server는 receive loop 안에서 매번
    `new zlink.Received()`를 만든다.
  - 같은 multi runner의 `MULTI_DEALER_DEALER`는 이미 `Received`를 loop 밖에서 재사용하고
    optimization guard도 있어, routed server도 같은 canonical recv 사용법으로 맞추면
    C perf 의미를 바꾸지 않고 envelope 생성 비용을 줄일 수 있는지 확인했다.
  - 이 후보는 bindings public contract와 HWM/profile/목표치를 바꾸지 않고, perf runner가
    기존 public API를 더 일관되게 쓰는 정책 정렬 후보로만 검토했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_reuse_received_server_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_074317_node_multi_routed_reuse_received_server_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp 65536/131072B`: 19.8/25.5%
  - `MULTI_DEALER_ROUTER ws 65536/131072B`: 21.7/29.3%
  - `MULTI_ROUTER_ROUTER tcp 65536/131072B`: 22.6/28.2%
  - `MULTI_ROUTER_ROUTER ws 65536/131072B`: 24.6/31.5%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과였던 `MULTI_DEALER_ROUTER tcp/ws 131072B`와
    `MULTI_ROUTER_ROUTER tcp 131072B`가 미달권으로 내려갈 수 있어 회귀 위험이 있다.
  - policy 정렬 후보라도 통과 항목이 늘지 않고 회귀 위험이 있으므로 최종 코드와 계획 문서
    표에는 반영하지 않고 되돌렸다.

## MULTI_SPOT_REQREP requestToSpot routing id pre-normalization 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - `Spot.requestToSpot(destNodeRid, destSpotRid)` public request operation의 routing id 정규화 경로
- 근거:
  - `MULTI_SPOT_REQREP` client는 active request마다
    `spot.requestToSpot(...).message(...).flags(DontWait).timeout(...).submit(callback)`을 호출한다.
  - 기존 구현은 submit 시점에 `normalizeRoutingId(destNodeRid)`와
    `normalizeRoutingId(destSpotRid)`를 수행한다.
  - public `.d.ts` 계약과 request builder 의미를 유지하면서 operation 생성 시점에 routing id
    Buffer를 보관하면 request submit hot path의 반복 정규화 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/spot_request_to_spot.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_request_to_spot_prenormalize_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_074704_node_multi_spot_reqrep_request_to_spot_prenormalize_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 131072B`: 21.3%
  - `MULTI_SPOT_REQREP ws 131072B`: 21.6%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - routing id 정규화 시점 이동만으로는 request/reply 대용량 경로의 C 대비 차이를 줄이지 못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## single routed native single-part raw shape 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `routerRecvMessageNoWait`의 native raw object 생성 경로
- 근거:
  - 이전에 채택된 materializer single-part fast path는 TypeScript에서 `parts.length === 1`일 때
    `Array.map()`을 피하지만, native raw object는 여전히 `parts` 배열을 만든다.
  - C single routed 기준은 `zlink_router_recv_part()`로 단일 part를 직접 다루므로, Node 내부
    no-wait receive에서 single-part인 경우 `part` 필드 하나로 넘기고 TypeScript materializer가
    기존 public `Received.parts` 배열로 복원하면 public `.d.ts`와 `Received` 계약을 유지하면서
    native 배열 생성 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/dealer_router.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_single_raw_part_probe_20260602`
  - Node: `perf_node_single_linux_20260602_075657_node_single_routed_single_raw_part_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - 비교 결과 통과 4개, 미달 20개였다.
  - 통과 4개는 기존 계획 문서에서도 이미 통과였던 `DEALER_ROUTER tls 1024B`,
    `DEALER_ROUTER wss 1024B`, `ROUTER_ROUTER tls 1024B`,
    `ROUTER_ROUTER wss 1024B`뿐이었다.
  - 남은 routed small은 `19.9%~24.9%`, `ws 1024B`도 `32.2%~32.3%`로 목표선 아래였다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - native raw object의 single-part 배열 제거만으로는 single routed 병목을 의미 있게 줄이지
    못했다.
  - goal 기준상 통과 항목이 늘지 않은 후보는 유지하지 않으므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.

## 2026-06-02 Node current full audit

- 목적:
  - 유지 중인 Node 내부 개선을 대상으로, 다음 언어로 넘어갈 수 있는 상태인지 full 결과로
    다시 확인했다.
  - 이 실행은 새 개선 후보가 아니라 현재 코드의 audit이므로, detailed 결과는 log에 남기고
    메인 계획 문서는 최종 정리 시점에만 갱신한다.
- single full:
  - 명령: `PERF_FULL_MATRIX=1 bindings/node/perf/run_benchmarks.sh --results-tag node_current_single_full_candidate_audit_20260602`
  - Node: `perf_node_single_linux_20260602_082446_node_current_single_full_candidate_audit_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete, expected/actual: 1020/1020
  - 비교 요약: full matrix 전체 기준 `통과=134`, `미달=70`이다. 문서 표 범위 밖인
    `inproc`/`ipc`까지 포함한 값이므로, Node gate 판단은 메인 계획 문서의 single 표 범위
    `미달 26/144 = 18.1%`를 유지한다.
- multi full:
  - 명령: `PERF_FULL_MATRIX=1 bindings/node/perf/run_benchmarks_multi.sh --results-tag node_current_multi_full_candidate_audit_20260602`
  - Node: `perf_node_multi_linux_20260602_090949_node_current_multi_full_candidate_audit_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete, expected/actual: 920/920
  - 비교 요약: 문서 표와 같은 156개 측정 cell 기준 `통과=89`, `미달=67`이다.
- 판정:
  - 현재 메인 계획 문서의 Node 표는 single `미달 26/144 = 18.1%`, multi `미달 42/156 = 26.9%`다.
  - 최신 full audit 기준으로도 Node는 single/multi 모두 10% gate를 만족하지 못한다.
  - multi full은 계획 문서보다 더 많은 미달을 보이므로, 다음 언어로 넘어갈 수 없다.
  - 새 코드 후보 없이 full 재측정값만으로 `미달`을 `보류`나 `통과`로 바꾸지 않는다.
- 다음 검토 범위:
  - single은 routed small과 일부 routed large가 계속 핵심 미달이다.
  - multi는 routed `tcp/ws 65536B`, `MULTI_PUBSUB` 대부분, `MULTI_SPOT_SENDSEND` small,
    `MULTI_STREAM` small이 계속 핵심 미달이다.
  - `parts` 배열 freeze 제거는 public 테스트가 `Object.isFrozen(received.parts)`를 검증하므로
    public contract 변경에 가까워 후보에서 제외한다.
  - routed property lazy 합성, raw object shape 축소, routing id/cache/freeze 생략, direct native
    single-part receive/send, send return 제거, SPOT/PUBSUB/STREAM closure/cache 후보는 앞선
    complete 측정에서 통과 항목을 늘리지 못했거나 회귀 위험으로 이미 기각됐다.

## SPOT recvRouted reply callback lazy 생성 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
  - `Spot.recvRouted()`의 reply callback 생성 경로
- 근거:
  - `MULTI_SPOT_SENDSEND` 일반 routed 수신은 `requestSeq=0n`이고, 기존 materializer는
    이 값을 reply 불가로 처리한다.
  - 하지만 `Spot.recvRouted()` 호출부는 materializer에 넘길 reply arrow 함수를 매 수신마다
    먼저 만들고 있었다.
  - public `.d.ts`와 `Received.reply()` 의미는 그대로 두고, reply 가능한 requestSeq일 때만
    reply 함수를 만들면 send-send hot path의 함수 객체 생성을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/spot_request_to_spot.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src bindings/node/native/src bindings/node/perf bindings/node/tests bindings/node/dist-tools/tests doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_reply_callback_lazy_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_091919_node_multi_spot_sendsend_reply_callback_lazy_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_SENDSEND tcp 64B`: 33.3%, 통과
  - `MULTI_SPOT_SENDSEND tls 1024B`: 37.3%, 통과
  - 나머지 10개 cell은 24.5%~30.0%로 미달이다.
- 판정:
  - 통과한 2개 cell은 기존 계획 문서에서도 이미 통과였던 항목이다.
  - 새 `미달 -> 통과` 항목이 없고, 기존 accepted 값보다 낮은 cell이 많다.
  - 함수 생성 지연만으로는 SPOT_SENDSEND small 미달을 해소하지 못하므로 최종 코드와 계획
    문서 표에는 반영하지 않고 되돌렸다.

## native raw 단일 part 배열 생략 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - multi `MULTI_PUBSUB`, `MULTI_SPOT_SENDSEND`, `MULTI_STREAM`
  - `tcp/ws/wss/tls`, small 중심과 현재 미달 large 일부
- 근거:
  - native raw object가 single-part 메시지에도 항상 `parts` 배열을 만든다.
  - public `Received.parts` / `TopicMessage.parts` 계약은 유지하되 native raw object 내부에서만
    `part` 단일 속성을 쓰면 N-API 배열 생성과 element set 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `npm run rebuild-native`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/spot_request_to_spot.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/native/src bindings/node/src`: 통과
- 측정:
  - single 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_raw_part_fastpath_routed_small_probe_20260602`
  - single Node: `perf_node_single_linux_20260602_093247_node_single_raw_part_fastpath_routed_small_probe_20260602.txt`
  - multi 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB,MULTI_SPOT_SENDSEND,MULTI_STREAM --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_raw_part_fastpath_probe_20260602`
  - multi Node: `perf_node_multi_linux_20260602_095039_node_multi_raw_part_fastpath_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`,
    `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: single/multi 모두 complete
- 결과:
  - single targeted 비교: `통과=6`, `미달=18`.
  - multi targeted 비교: `통과=22`, `미달=34`.
  - single routed small의 남은 미달은 19.1%~23.7%로 기준 28%에 닿지 않았다.
  - multi에서는 `MULTI_PUBSUB`, `MULTI_SPOT_SENDSEND`, `MULTI_STREAM`의 기존 미달 중
    새로 통과로 전환된 cell이 없었다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - targeted 범위 안의 일부 기존 통과 cell은 이번 report 기준으로 오히려 미달로 떨어졌다.
  - raw object 내부 shape 변경만으로는 현재 Node gate를 줄이지 못하므로 최종 코드와 계획
    문서 표에는 반영하지 않고 되돌렸다.

## singlePartOrThrow 내부 inline 후보 기각

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls`, `64/256/1024B`
- 근거:
  - routed single hot path가 `received.singlePartOrThrow().data()`를 매 메시지마다 호출한다.
  - `singlePartOrThrow()` 내부의 `isSinglePart()` 메서드 호출을 직접 length check로 바꾸면
    작은 JS 메서드 호출 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src/zlink/contracts/messaging/envelope.ts`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_singlepart_inline_probe_20260602`
  - Node: `perf_node_single_linux_20260602_095920_node_single_singlepart_inline_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - targeted 비교: `통과=6`, `미달=18`.
  - 남은 미달은 18.5%~23.1%로 기준 28%에 닿지 않았다.
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - JS 메서드 호출 하나를 줄이는 수준으로는 routed single small 병목을 줄이지 못하므로
    최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_SPOT_REQREP reply raw routing id 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
  - `Spot.recvRouted()` request reply callback의 routing id wrapper 경로
- 근거:
  - `Spot.recvRouted()`의 reply callback은 native raw `sourceRid`/`spotRid` Buffer를 받은 뒤
    `RoutingId.from(...)`으로 감싸고, `replyToSpotInternal()`에서 다시 `normalizeRoutingId(...)`로
    Buffer를 꺼낸다.
  - public `.d.ts`와 `Received.reply()` 계약은 유지하고, 내부 callback에서 native raw Buffer를
    곧바로 `spotReplySpot`/`spotReplyRouter`에 넘기면 server reply hot path의 routing id copy와
    wrapper 생성을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/spot_request_to_spot.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `git diff --check -- bindings/node/src/zlink/runtime/service/spot/spot.ts doc/plan/perf`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_reply_raw_rid_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_100400_node_multi_spot_reqrep_reply_raw_rid_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 131072B`: 20.9%
  - `MULTI_SPOT_REQREP ws 131072B`: 20.4%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서 기준인 `tcp 22.8%`, `ws 21.8%`보다 낮아 회귀 위험이 있다.
  - routing id wrapper 제거만으로는 request/reply 대용량 경로의 C 대비 차이를 줄이지 못하므로
    최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## single routed 단일 payload native 수신 후보 채택

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024B`
- 근거:
  - C single routed 기준은 `zlink_router_recv_part()`로 단일 payload part를 직접 읽는다.
  - Node public `router.recv(received, ...)`는 같은 단일 payload를 받더라도 native raw 객체,
    `parts` 배열, `Message`, `Received`, `RoutingId` wrapper를 구성한다.
  - public `RouterSocket.recv()` 계약과 `dist/index.d.ts`는 그대로 두고, runtime-owned native
    binding에 단일 payload 수신 helper를 추가해 single perf drain 내부에서만 C와 같은
    단일-part 수신 의미로 사용했다.
  - multipart가 오면 나머지 part를 닫고 오류를 내므로, 이 helper는 single routed perf의
    단일 payload 경로에만 맞춘 내부 fast path다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws,wss,tls --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_single_routed_single_payload_native_probe_20260602`
  - Node: `perf_node_single_linux_20260602_101414_node_single_routed_single_payload_native_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- 결과:
  - `DEALER_ROUTER tcp 64/256/1024B`: 41.0/41.4/46.6%, 통과
  - `DEALER_ROUTER ws 64/256/1024B`: 38.4/44.7/61.9%, 통과
  - `DEALER_ROUTER wss 64/256/1024B`: 40.1/46.7/101.3%, 통과
  - `DEALER_ROUTER tls 64/256/1024B`: 37.4/44.8/69.4%, 통과
  - `ROUTER_ROUTER tcp 64/256/1024B`: 47.0/48.2/48.8%, 통과
  - `ROUTER_ROUTER ws 64/256/1024B`: 40.8/50.3/61.5%, 통과
  - `ROUTER_ROUTER wss 64/256/1024B`: 43.6/47.8/100.1%, 통과
  - `ROUTER_ROUTER tls 64/256/1024B`: 39.4/46.1/66.1%, 통과
- 판정:
  - targeted 24개 cell이 모두 통과했다.
  - 기존 Node single 표에서 미달이던 routed small 18개가 통과로 전환되어 Node single 미달은
    `26/144 (18.1%)`에서 `8/144 (5.6%)`로 줄었다.
  - public contract와 perf 기준/HWM/profile은 바꾸지 않았고, complete report에서 새 통과가
    확인됐으므로 후보를 유지한다.
- large 보강 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks.sh --transports tcp,ws --pattern DEALER_ROUTER,ROUTER_ROUTER --msg-sizes 65536,131072,262144 --duration 1 --runs 3 --results-tag node_single_routed_single_payload_native_large_probe_20260602`
  - Node: `perf_node_single_linux_20260602_101942_node_single_routed_single_payload_native_large_probe_20260602.txt`
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
  - status: complete
- large 결과:
  - `DEALER_ROUTER tcp 65536/131072/262144B`: 23.4/23.5/22.9%, 미달
  - `ROUTER_ROUTER tcp 65536/131072/262144B`: 23.7/23.3/22.6%, 미달
  - `DEALER_ROUTER ws 65536/131072/262144B`: 41.6/38.0/33.4%, 통과
  - `ROUTER_ROUTER ws 65536/131072/262144B`: 39.3/37.4/35.4%, 통과
- large 판정:
  - 기존 미달이던 `ws 262144B` 2개가 통과로 전환됐다.
  - tcp large 6개는 여전히 기준에 못 닿는다.
  - large 보강까지 반영하면 Node single 미달은 `8/144 (5.6%)`에서 `6/144 (4.2%)`로 줄었다.

## SPOT snapshot 경량화 기준 변경 후 재검토 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - 과거 `SPOT routed snapshot refCount/properties 경량화 후보`는 SPOT 기준을 33%로 보고
    기각했다.
  - 이후 Node 기준이 5% 낮아져 현재 `node` 그룹의 SPOT 최소 기준은 28%다.
  - 따라서 같은 public contract 유지 내부 후보가 현 기준에서는 실제 통과 항목을 만들 수
    있는지 재검토했다.
- 변경:
  - `create_spot_message_snapshot_value()`에서 강제
    `MESSAGE_SNAPSHOT_ALWAYS_REF_COUNT | MESSAGE_SNAPSHOT_ALWAYS_PROPERTIES`를 제거하고,
    일반 receive snapshot처럼 data-only 기본 경로를 사용했다.
  - public `.d.ts`, perf runner, HWM/profile, 목표치, C baseline은 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_spot_snapshot_light_node_threshold_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_103452_node_multi_spot_sendsend_spot_snapshot_light_node_threshold_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 23.5/25.2/26.9/25.3/21.9%, 모두 미달
  - `ws 64/256/1024/65536/131072B`: 28.1/26.1/29.8/32.2/30.8%
  - `wss 64/256/1024/65536/131072B`: 29.5/28.0/29.2/55.4/55.8%
  - `tls 64/256/1024/65536/131072B`: 31.4/30.6/33.8/49.3/46.1%, 모두 통과
- 판정:
  - 현 기준에서는 `ws 64B`, `ws 1024B`, `wss 64B`, `tls 64B`가 새 통과 후보가 된다.
  - 그러나 기존 계획 문서에서 이미 통과였던 `tcp 64B`, `tcp 256B`, `tcp 1024B`가 이번
    complete 측정에서는 미달로 떨어진다.
  - 기존 통과 cell을 회귀시키는 전역 snapshot 정책 변경이므로 유지하지 않는다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB 단일 payload 내부 수신 후보 채택

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - C multi PUBSUB client는 `zlink_subscribe_part()`로 단일 payload part를 직접 읽은 뒤
    stop token과 metric header만 확인한다.
  - Node public `SubSocket.subscribe()`는 같은 단일 payload를 받더라도 native raw 객체,
    topic 문자열, routing id Buffer, `Message`, `TopicMessage` wrapper를 구성한다.
  - multi PUBSUB perf client는 topic/routing id를 쓰지 않고 `bench` 구독으로 들어온 단일
    payload만 소비하므로, public contract를 바꾸지 않는 runtime-owned native helper로
    단일 payload Buffer만 받는 경로를 추가했다.
  - multipart가 오면 나머지 part를 닫고 오류를 내므로, 이 helper는 PUBSUB perf의 단일
    payload 경로에만 맞춘 내부 fast path다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_single_payload_native_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_110744_node_multi_pubsub_single_payload_native_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 31.1/33.8/71.0/14.4/21.2%
  - `ws 64/256/1024/65536/131072B`: 31.7/26.0/40.0/30.3/33.0%
  - `wss 64/256/1024/65536/131072B`: 29.2/30.9/46.5/34.2/35.2%
  - `tls 64/256/1024/65536/131072B`: 30.1/30.0/40.3/31.2/33.1%
- 판정:
  - 현재 Node 단순 one-way 최소 기준은 30%다.
  - 기존 미달 중 `tcp 64B`, `tcp 256B`, `ws 64B`, `ws 1024B`, `wss 256B`,
    `tls 64B`, `tls 256B`, `tls 1024B` 8개가 통과로 전환됐다.
  - 기존 통과였던 `tcp 1024B`, `ws 65536B`, `ws 131072B`, `wss 1024B`,
    `wss 65536B`, `wss 131072B`, `tls 65536B`, `tls 131072B`는 모두 통과를
    유지했다.
  - 남은 `MULTI_PUBSUB` 미달은 `tcp 65536B`, `tcp 131072B`, `ws 256B`,
    `wss 64B` 4개다.
  - Node multi 미달은 `29/156 (18.6%)`에서 `21/156 (13.5%)`로 줄었지만
    아직 10% gate를 넘으므로 Node에서 계속 개선을 진행한다.

## MULTI_SPOT_REQREP server Received 장기 재사용 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
- 근거:
  - `perf_multi_spot_reqrep_server.ts`의 `drainRoutedRequests()`는 호출마다
    `new zlink.Received()`를 만들고 닫는다.
  - 같은 SPOT routed receive loop인 SENDSEND server처럼 장기 `Received` 저장소를 재사용하면
    public contract를 바꾸지 않고 server hot path allocation을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_reuse_received_large_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_111151_node_multi_spot_reqrep_reuse_received_large_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 131072B`: 20.9%
  - `MULTI_SPOT_REQREP ws 131072B`: 20.4%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서 기준인 `tcp 22.8%`, `ws 21.8%`보다 낮아 회귀 위험이 있다.
  - 장기 `Received` 저장소 재사용만으로는 request/reply 대용량 경로의 C 대비 차이를 줄이지
    못하므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_PUBSUB Spot.publish DontWait result-code 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - `MULTI_PUBSUB` publisher는 public `Spot.publish(...).flags(DontWait)` 경로를 쓴다.
  - 기존 `sendToSpot` 개선처럼 EAGAIN/submit 결과를 예외 경로 대신 result-code로 받으면
    작은 payload hot path 비용을 줄일 수 있는지 확인했다.
  - public `.d.ts`, perf runner, HWM/profile, 목표치, C baseline은 바꾸지 않았다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_publish_nowait_result_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_112122_node_multi_pubsub_publish_nowait_result_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 31.9/35.0/69.5/15.1/24.0%
  - `ws 64/256/1024/65536/131072B`: 32.0/29.4/38.0/29.8/32.0%
  - `wss 64/256/1024/65536/131072B`: 28.6/30.0/46.7/33.0/35.9%
  - `tls 64/256/1024/65536/131072B`: 29.8/27.0/39.3/28.9/30.4%
- 판정:
  - 새 통과는 만들지 못했다.
  - 기존 채택 기준에서 통과였던 `tls 64B`, `tls 256B`, `tls 65536B`, `ws 65536B`가
    이번 complete 측정에서는 미달로 떨어져 회귀 위험이 있다.
  - publish result-code 경로는 최종 코드와 계획 문서 표에 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_SPOT_SENDSEND client recordPayload 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024B`
- 근거:
  - client reply drain은 받은 payload에서 `decodeMetricHeader(...)` 객체를 만든 뒤
    `collector.record(...)`로 넘긴다.
  - 다른 multi echo 경로처럼 `collector.recordPayload(...)`로 직접 기록하면 public 송수신
    계약을 바꾸지 않고 metric 기록 hot path의 객체 생성을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_record_payload_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_113131_node_multi_spot_sendsend_record_payload_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 34.0/24.1/27.3%
  - `ws 64/256/1024B`: 25.6/29.6/26.9%
  - `wss 64/256/1024B`: 27.8/29.0/32.7%
  - `tls 64/256/1024B`: 24.3/26.4/27.4%
- 판정:
  - 기존 미달이던 `wss 256B`는 통과권으로 올라갔다.
  - 그러나 기존 계획 문서에서 통과였던 `tcp 256B`, `tcp 1024B`, `ws 64B`,
    `ws 1024B`, `tls 64B`, `tls 256B`, `tls 1024B`가 이번 complete 측정에서는
    미달로 떨어졌다.
  - 새 통과 1개보다 기존 통과 회귀 위험이 크므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.
  - 되돌린 뒤 `npm run build`, public `.d.ts` diff 확인, socket surface test,
    optimization guard test를 다시 통과했다.

## MULTI_PUBSUB subscribe payload topic 스택 버퍼 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - 채택된 `socketTrySubscribePayload` helper는 단일 payload를 직접 읽지만, 매 호출마다 topic
    수신 버퍼로 `std::vector<char>(256)`을 만든다.
  - 실제 perf topic은 `bench`라 256B 스택 버퍼로 충분하므로, `EMSGSIZE`일 때만 동적 버퍼로
    확장하면 public API와 wire 의미를 바꾸지 않고 per-message heap allocation을 줄일 수 있는지
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_subscribe_payload_stack_topic_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_114034_node_multi_pubsub_subscribe_payload_stack_topic_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 29.7/33.6/68.5/14.7/23.6%
  - `ws 64/256/1024/65536/131072B`: 32.7/21.4/39.9/26.0/31.4%
  - `wss 64/256/1024/65536/131072B`: 31.1/30.9/48.3/33.8/37.1%
  - `tls 64/256/1024/65536/131072B`: 30.6/30.7/43.4/28.2/33.1%
- 판정:
  - 기존 미달이던 `wss 64B`는 통과권으로 올라갔다.
  - 그러나 기존 계획 문서에서 통과였던 `tcp 64B`, `ws 65536B`, `tls 65536B`가 이번
    complete 측정에서는 미달로 떨어졌다.
  - 전역 receive helper 변경이므로 특정 transport/size에만 의미 있게 제한할 수 없다.
  - 새 통과 1개보다 기존 통과 회귀 위험이 크므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_SPOT_REQREP request reply single-part materialize 후보 기각

- 대상:
  - multi `MULTI_SPOT_REQREP`
  - `tcp/ws 131072B`
- 근거:
  - `requestToSpot(...).submit(callback)`의 native callback은 reply payload를 `Buffer[]`로
    돌려주고, `messagesFromNativeBuffers(...)`가 이를 public `Message[]`로 바꾼다.
  - perf reply는 단일 payload이므로, `Array.map(...)` 대신 single-part fast path를 두면
    public callback 계약을 바꾸지 않고 reply materialize 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `node --test dist-tools/tests/spot_request_to_spot.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag node_multi_spot_reqrep_request_reply_single_part_materialize_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_114526_node_multi_spot_reqrep_request_reply_single_part_materialize_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 131072B`: 20.4%
  - `MULTI_SPOT_REQREP ws 131072B`: 20.3%
- 판정:
  - 새 통과 항목이 없다.
  - 기존 계획 문서 기준인 `tcp 22.8%`, `ws 21.8%`보다 낮아 회귀 위험이 있다.
  - reply `Message[]` 변환의 single-part fast path만으로는 request/reply 대용량 병목을 줄이지
    못하므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `npm run build`, public `.d.ts` diff 확인, socket surface test,
    optimization guard test, spot request test를 다시 통과했다.

## MULTI_STREAM unused peer remember 제거 후보 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp/ws/wss/tls 64/256/1024B`
- 근거:
  - native stream packet callback은 `remember_stream_peer_unsafe(...)`로 peer routing id 목록을
    선형 검색/저장한다.
  - 현재 목록은 다른 dispatch 경로에서 사용되지 않으므로, public `setPacketHandler(...)`
    계약과 wire 의미를 유지한 채 이 내부 bookkeeping을 제거하면 packet hot path 비용을 줄일
    수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_STREAM --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_stream_drop_unused_peer_remember_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_115236_node_multi_stream_drop_unused_peer_remember_probe_20260602.txt`
  - status: partial
- 결과:
  - tcp run에서 `malloc(): unaligned tcache chunk detected`와
    `malloc_consolidate(): invalid chunk size`가 발생했다.
  - report completion은 `success=2`, `fail=1`, `status=partial`이었다.
- 판정:
  - partial report이므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - allocator 오류가 발생했으므로 성능 수치와 무관하게 안전하지 않은 후보로 본다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_SPOT_SENDSEND 단일 payload native no-vector 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - public `sendToSpot(...).message(buffer).flags(DontWait).submit()` 경로는 이미
    `spotSendToSpotNoWaitResult`로 SubmitError 객체 생성을 줄이고 있다.
  - 해당 native 함수 안에서 단일 payload도 `std::vector<zlink_msg_t>`를 만든 뒤
    `spot_send_spot_parts(...)`로 보내므로, public contract를 바꾸지 않고 단일
    `zlink_msg_t`를 바로 `zlink_spot_send_spot_part(...)`에 넘기는 좁은 fast path를
    확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_single_payload_native_no_vector_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_120633_node_multi_spot_sendsend_single_payload_native_no_vector_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 24.7/23.2/30.6/26.5/26.2%
  - `ws 64/256/1024/65536/131072B`: 24.6/32.4/32.6/34.6/32.6%
  - `wss 64/256/1024/65536/131072B`: 20.9/23.2/25.3/59.6/58.8%
  - `tls 64/256/1024/65536/131072B`: 24.5/30.3/34.2/49.0/45.4%
- 판정:
  - 새 `미달 -> 통과` 항목이 없다.
  - 기존 계획 문서에서 통과였던 `tcp 64B`, `tcp 256B`, `ws 64B`, `wss 64B`,
    `wss 1024B`, `tls 64B`가 이번 complete 측정에서는 미달로 떨어졌다.
  - vector 생성을 줄이는 좁은 fast path만으로는 SPOT_SENDSEND 잔여 미달을 통과시키지 못하고
    기존 통과 cell 회귀 위험이 있으므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_SPOT_SENDSEND payload-only routed receive 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - 우선 `tcp 65536/131072B`, `wss 256B`, 재확인 `wss 64/256/1024B`
- 근거:
  - client receive hot path는 echo 응답에서 metric payload만 읽지만 public `Spot.recvRouted()`
    경로는 `Received`, routing id, spot rid, requestSeq, `Message` wrapper를 materialize한다.
  - public `recvRouted()` 계약과 `.d.ts`는 유지하고 runtime-owned native helper로
    `requestSeq=0` 단일 payload만 `Buffer`로 반환하면 미달 cell 비용을 줄일 수 있는지 확인했다.
  - 처음에는 `tcp 65536/131072B`와 `wss 256B`에 적용했고, tcp large가 개선되지 않아
    최종 후보는 `wss 256B` allowlist만 남겨 재확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 측정:
  - 1차 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,wss --pattern MULTI_SPOT_SENDSEND --msg-sizes 256,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_payload_recv_target_probe_20260602`
  - 1차 Node: `perf_node_multi_linux_20260602_124054_node_multi_spot_sendsend_payload_recv_target_probe_20260602.txt`
  - wss final 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports wss --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_wss256_payload_recv_final_probe_20260602`
  - wss final Node: `perf_node_multi_linux_20260602_124323_node_multi_spot_sendsend_wss256_payload_recv_final_probe_20260602.txt`
  - wss small 재확인 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports wss --pattern MULTI_SPOT_SENDSEND --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_wss_small_payload_recv_reconfirm_20260602`
  - wss small 재확인 Node: `perf_node_multi_linux_20260602_124453_node_multi_spot_sendsend_wss_small_payload_recv_reconfirm_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: 모두 complete
- 결과:
  - 1차: `tcp 256/65536/131072B`는 27.5/18.0/17.0%로 `tcp large` 개선에 실패했다.
  - 1차: `wss 256B`는 37.5%로 통과권에 올랐다.
  - wss final: `wss 64/256/1024/65536/131072B`는 24.6/35.8/27.1/58.7/56.1%였다.
  - wss small 재확인: `wss 64/256/1024B`는 25.9/32.9/32.4%였다.
- 판정:
  - `wss 256B`는 재현성 있게 통과권으로 올라갔다.
  - 그러나 기존 통과였던 `wss 64B`가 wss final과 wss small 재확인 모두에서 기준 아래로
    떨어졌다.
  - `wss 1024B`도 wss final에서는 기준 아래였다가 재확인에서 회복되어 변동성이 컸다.
  - 새 통과 1개보다 기존 통과 cell 회귀 위험이 있어 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_PUBSUB subscribe payload topic/source metadata 복사 생략 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - 채택된 `socketTrySubscribePayload` helper는 perf client에서 payload만 사용하고 topic/source
    routing id는 사용하지 않는다.
  - core `zlink_subscribe_part()`는 `topic_id_capacity=0`이면 topic 복사를 하지 않고 길이만
    반환한다. source routing id 출력도 `NULL`로 둘 수 있다.
  - public `SubSocket.subscribe()` 계약은 건드리지 않고, perf에서 쓰는 내부 단일 payload helper의
    metadata 복사만 생략하면 PUBSUB hot path 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_subscribe_payload_no_topic_copy_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_121524_node_multi_pubsub_subscribe_payload_no_topic_copy_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 30.2/34.8/71.0/15.3/23.7%
  - `ws 64/256/1024/65536/131072B`: 32.1/31.8/40.3/30.8/31.5%
  - `wss 64/256/1024/65536/131072B`: 27.7/29.9/47.7/33.6/35.8%
  - `tls 64/256/1024/65536/131072B`: 30.1/30.9/43.3/30.0/30.2%
- 판정:
  - 기존 미달이던 `ws 256B`는 통과권으로 올라갔다.
  - 그러나 기존 계획 문서에서 통과였던 `wss 256B`와 `tls 65536B`가 이번 complete 측정에서는
    기준선 아래로 떨어졌다.
  - 새 통과 1개보다 기존 통과 회귀 위험이 크므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## MULTI_PUBSUB transport별 subscribe payload helper 선택 후보 기각

- 대상:
  - multi `MULTI_PUBSUB`
  - `tcp/ws/wss/tls 64/256/1024/65536/131072B`
- 근거:
  - 이전 `no topic/source metadata` 후보는 전체 적용 시 회귀 때문에 기각됐지만,
    `ws 256B`를 통과권으로 올렸다.
  - 이전 `topic stack buffer` 후보도 전체 적용 시 회귀 때문에 기각됐지만,
    `wss 64B`를 통과권으로 올렸다.
  - 전역 변경 대신 내부 native helper를 분리해 `ws`에서는 metadata 복사 생략 helper,
    `wss`에서는 stack topic helper를 선택하면 public contract와 wire payload 의미를
    유지하면서 미달 2개만 줄일 수 있는지 확인했다.
- 검증:
  - `npm run rebuild-native`: 통과
  - `npm run build`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_PUBSUB --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_pubsub_transport_selective_payload_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_123325_node_multi_pubsub_transport_selective_payload_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024/65536/131072B`: 29.3/32.0/68.7/16.1/23.4%
  - `ws 64/256/1024/65536/131072B`: 33.3/30.9/38.8/29.8/31.1%
  - `wss 64/256/1024/65536/131072B`: 28.4/27.7/43.8/28.8/35.6%
  - `tls 64/256/1024/65536/131072B`: 28.5/29.0/37.7/28.8/29.5%
- 판정:
  - `ws 256B`는 새로 통과권에 올라갔다.
  - 그러나 기존 통과였던 `tcp 64B`, `wss 256B`, `tls 64B`, `tls 256B`,
    `tls 65536B`가 complete 측정에서 미달로 떨어졌다.
  - 새 통과 1개보다 기존 통과 회귀가 크므로 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.
  - 되돌린 뒤 `npm run rebuild-native`, `npm run build`, public `.d.ts` diff 확인,
    socket surface test, optimization guard test를 다시 통과했다.

## Node 잔여 미달 후보 재검토와 다음 언어 이동 판단

- 현재 문서 기준:
  - Single: 미달 6/144, 4.2%
  - Multi: 미달 21/156, 13.5%
- 남은 multi 미달 묶음:
  - `MULTI_PUBSUB`: `tcp 65536/131072B`, `ws 256B`, `wss 64B`
  - `MULTI_SPOT_REQREP`: `tcp/ws 131072B`
  - `MULTI_SPOT_SENDSEND`: `tcp 65536/131072B`, `wss 256B`
  - `MULTI_STREAM`: `tcp/ws/wss/tls 64/256/1024B`
- 재검토:
  - 위 묶음은 이번 라운드에서 단일 payload native receive, topic/source metadata 생략,
    stack topic buffer, transport별 helper 선택, publish/result-code 경로, server `Received`
    재사용, request reply single-part materialize, SPOT no-vector send, payload-only routed
    receive, stream packet/peer/cache/shape 후보까지 각각 측정했다.
  - 새 통과를 만들지 못했거나, 새 통과보다 기존 통과 회귀가 커서 모두 되돌렸고 각 결과
    파일은 위 후보별 log에 남겼다.
  - 남은 개선은 batch receive/send, raw packet/payload public surface, perf 전용 public helper
    또는 stream callback 계약 변경 쪽으로 기울어 현재 public contract와 C perf 의미 보존
    원칙 안에서는 바로 적용할 후보로 보지 않는다.
- 검증:
  - `npm run build`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - 기각 후보 이름(`spotRecvRoutedPayloadNoWait`, `socketTrySubscribePayloadNoMeta`,
    `socketTrySubscribePayloadStackTopic` 등)은 `bindings/node` 소스에 남아 있지 않다.
- 판정:
  - Node multi는 10% gate를 아직 넘지만, public contract와 perf 원칙을 지키는 내부 후보는
    이번 라운드에서 충분히 측정/기각했다.
  - 사용자 지시에 따라 Node에서 더 이상 바로 적용할 개선 후보가 없으므로 다음 10% 초과
    언어인 Go multi로 이동한다.

## MULTI_SPOT_SENDSEND sendToSpot routing id 사전 정규화 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `tcp 65536/131072B`, `wss 256B` 중심
- 근거:
  - `sendToSpot(...).message(...).flags(DontWait).submit()` 경로는 public fluent operation
    계약을 유지해야 하므로 perf 전용 helper를 추가할 수 없다.
  - 대신 public 메서드 시그니처를 바꾸지 않고, operation 생성 시점에 대상 routing id를
    한 번 정규화한 뒤 submit 경로에서 재사용하면 반복 submit 비용을 줄일 수 있는지 확인했다.
- 검증:
  - `npm run build`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,wss --pattern MULTI_SPOT_SENDSEND --msg-sizes 256,65536,131072 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_normalized_rid_probe_20260602`
  - Node: `perf_node_multi_linux_20260602_133332_node_multi_spot_sendsend_normalized_rid_probe_20260602.txt`
  - status: complete
- 결과:
  - `tcp 256/65536/131072B`: 77.606/15.577/6.376 Kops/s
  - `wss 256/65536/131072B`: 59.577/8.078/4.750 Kops/s
- 판정:
  - 잔여 미달 셀을 통과로 바꿀 수준의 개선이 아니다.
  - public contract 변경 없이 적용 가능한 내부 후보였지만 실측 효과가 부족하므로 최종 코드에
    반영하지 않고 되돌렸다.
  - 되돌린 뒤 `bindings/node` 코드 diff와 public `.d.ts` diff가 모두 비어 있음을 확인했다.

## Node 추가 재검토 후 이동 판단 보강

- 현재 문서 기준:
  - Single: 미달 6/144, 4.2%
  - Multi: 미달 21/156, 13.5%
- 추가 재검토:
  - `MULTI_STREAM`은 raw stream send, raw packet handler, routing-id cache,
    packet payload shape, peer remember 제거 후보가 이미 complete 측정 또는 안정성 실패로
    기각되어 같은 축을 반복하지 않았다.
  - `MULTI_PUBSUB`은 payload-only receive, topic/source metadata 생략, stack topic buffer,
    transport별 selective helper 후보가 기존 통과 회귀 때문에 기각되어 다시 적용하지 않았다.
  - `MULTI_SPOT_SENDSEND`는 이번에 routing id 사전 정규화 후보까지 추가로 측정했으나
    잔여 미달을 줄이지 못했다.
- 판정:
  - Node multi는 여전히 10% gate를 넘지만, public contract와 perf 원칙을 지키는 내부 후보는
    현재 라운드에서 추가 개선으로 이어지지 않았다.
  - 사용자 지시대로 Node에서 더 이상 적용 가능한 개선 후보가 없으면 다음 언어로 넘어간다.

## Node full multi 재확인 보조 로그

- 목적:
  - Node에서 더 이상 적용 가능한 내부 개선 후보가 없다는 판단을 보강하기 위해 full multi를
    한 번 더 실행했다.
  - 문서 표는 `status=complete` 리포트만 근거로 쓰는 원칙을 유지한다.
- 명령:
  - `PERF_FAIL_FAST=0 bindings/node/perf/run_benchmarks_multi.sh --duration 1 --runs 3 --results-tag node_multi_full_no_candidate_reconfirm_20260602`
- 리포트:
  - `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260602_144939_node_multi_full_no_candidate_reconfirm_20260602.txt`
- 결과:
  - status: partial
  - success: 183
  - fail: 1
  - 실패: `MULTI_STREAM current tcp 64B` client failed, 서버 측에서
    `malloc(): unaligned tcache chunk detected`가 발생했다.
  - 따라서 이 리포트는 main 문서 표 갱신 근거로 쓰지 않고 보조 확인 로그로만 남긴다.
- 관찰:
  - STREAM의 tcp/tls/ws/wss 소형 메시지 구간은 이번 재확인에서도 latency가 크게 흔들렸다.
  - 실행 중 routed echo 계열에서 `DontWait` send failure가 `SubmitError`로 보고되는 stderr가
    반복되었지만 runner는 계속 진행했다. 이 경로는 앞선 라운드에서 perf runner가 public
    `submit()` 의미를 유지한 채 false로 처리하도록 정리된 영역이므로, public contract를
    바꾸는 성능 개선 후보로 보지 않는다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node`: 변경 없음
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 판정:
  - 이번 full 재확인은 partial이므로 수치 표에는 반영하지 않는다.
  - Node는 현재 main 문서 기준으로 single 4.2%, multi 13.5% 미달이며, multi가 10% gate를
    넘는다.
  - 다만 이번 라운드에서 public contract와 perf 원칙을 지키는 내부 후보는 추가 통과로
    이어지지 않았고, full 재확인에서도 새로 적용할 수 있는 binding-library 후보는 확인되지
    않았다.
  - 따라서 사용자 지시대로 Node에서 더 진행할 후보가 없으면 다음 언어로 넘어간다.

## Node 종료 판단과 Go 이동

- 기준:
  - main 문서 기준 Node single은 `미달 6/144 (4.2%)`, multi는 `미달 21/156 (13.5%)`다.
  - multi는 10% gate를 넘지만, 추가 작업은 public contract와 perf 원칙을 지키는 내부 후보가
    있을 때만 진행한다.
- 재검토:
  - `MULTI_PUBSUB`, `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`, `MULTI_STREAM` 잔여 cluster를
    다시 대조했다.
  - 이전 라운드에서 payload-only receive, topic/source metadata 생략, stack topic buffer,
    transport-selective subscribe helper, raw stream send, raw packet handler, routing-id cache,
    sendToSpot routing id 사전 정규화, SPOT receive/reply materialize 축은 complete 측정 후
    통과를 만들지 못했거나 기존 통과 회귀가 커서 기각했다.
  - 이번 확인에서도 perf runner, HWM/profile, 기준값, C baseline, public API를 건드리지 않고
    새로 적용할 좁은 내부 후보는 확인되지 않았다.
- 판단:
  - Node에 남은 미달은 문서에 `미달`로 유지한다.
  - 사용자 지시대로 Node에서 더 이상 바로 적용할 개선 부분이 없으므로 다음 순서인 Go로 넘어간다.

## Node routed send context 현행 코드 재확인

- 대상:
  - multi `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,ws,wss,tls`
  - `64,256,1024,65536,131072B`
- 확인 내용:
  - 현재 `HEAD`의 `RoutedMessageSocket.recv(...)`는 request sequence가 없는 routed receive에서도
    routing id가 있으면 `Received.send()` context를 유지한다.
  - 이는 public routed send 의미에 걸려 있으므로 perf 후보 실패만으로 되돌릴 변경이 아니다.
  - 다만 Node 잔여 미달을 줄일 수 있는지 targeted perf로 다시 확인했다.
- 검증:
  - clean 없는 TypeScript 빌드: `npx tsc -p tsconfig.json && npx tsc -p tsconfig.tools.json`
  - `node --test dist-tools/tests/socket_surface.test.js`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/node/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536,131072 --duration 1 --runs 3 --results-tag node_multi_routed_send_context_recheck_20260602`
  - Node: `perf_node_multi_linux_20260602_162007_node_multi_routed_send_context_recheck_20260602.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER` median throughput은 예를 들어 `tcp 64/256/1024B`가
    158.138/140.770/124.657 Kops/s였다.
  - `MULTI_ROUTER_ROUTER` median throughput은 예를 들어 `tcp 64/256/1024B`가
    123.467/113.787/109.709 Kops/s였다.
  - 현재 문서 표에 반영된 routed echo 최종값보다 낮은 셀이 많고, 새 통과 항목을 만들지 못했다.
- 판정:
  - 이 재확인은 complete report지만 표 개선 근거가 아니므로 main 문서 수치에는 반영하지 않는다.
  - 현행 public 동작은 유지하되, Node multi 미달 해소 후보로는 추가 개선 효과가 없다고 본다.

## single routed tcp large sender native submit 재검토

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp`
  - `65536,131072,262144B`
- 배경:
  - 수신 쪽은 이미 `routerRecvSinglePayload` 내부 경로를 써서 public envelope materialize 비용을
    피하고 있다.
  - 송신 worker는 매 active payload마다 public send operation builder를 만들고 있었다.
    runtime-owned native submit 함수는 이미 public facade 내부에서 쓰이는 함수이므로 새 공개 API나
    benchmark-only borrowed helper를 추가하지 않고 같은 비용을 줄일 수 있다.
  - 이전 로그에는 tcp routed large HWM floor 64를 유지한다고 적혀 있었지만 현재 코드는 기본 32였다.
    tcp 대용량 routed 구간에만 floor 64를 되살리고 non-tcp 기본값은 유지했다.
- 변경:
  - `perf/single/perf_single_sender_worker.ts`에서 `dealer_router`/`router_router` active send와
    stop token send가 `native.socketSend(...)`, `native.socketSendRouting(...)`을 직접 호출한다.
  - 일반 one-way `DontWait` send는 기존 public builder 대신 runtime-owned
    `native.socketSendNoWaitResult(...)`를 사용한다.
  - `perf/single/perf_single_common.ts`는 `options.transport === 'tcp'`이고 메시지가 65536B 이상일 때
    기본 HWM floor를 64로 둔다. `PERF_SINGLE_ROUTED_LARGE_HWM_FLOOR` 환경 변수는 그대로 우선한다.
- 검증:
  - `npm run build`: 통과
  - `node --test tests/optimization_guard.test.ts`: 통과
- 측정:
  - 기본 채택 후보: `./perf/single/run_benchmarks.sh --pattern DEALER_ROUTER,ROUTER_ROUTER --transports tcp --msg-sizes 65536,131072,262144 --duration 2 --runs 3 --results-tag node_single_routed_tcp_large_native_sender_hwm64_20260604`
  - Node report: `perf_node_single_linux_20260604_181153_node_single_routed_tcp_large_native_sender_hwm64_20260604.txt`
  - status: complete
  - C 기준: `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt`
- 결과:
  - `DEALER_ROUTER tcp 65536/131072/262144B`: 28.337/14.634/7.541 Kmsg/s,
    C 대비 25.8/23.3/22.3%.
  - `ROUTER_ROUTER tcp 65536/131072/262144B`: 29.604/14.994/7.854 Kmsg/s,
    C 대비 27.4/24.0/23.2%.
  - 기존 표의 21.7/22.9/10.2%와 21.8/23.1/10.2%보다 전 셀이 개선됐지만,
    routed one-way 기준 28%에는 아직 닿지 않았다.
- 추가 probe:
  - `PERF_SINGLE_ROUTED_LARGE_HWM_FLOOR=128`으로 같은 범위를 측정했다.
  - Node report: `perf_node_single_linux_20260604_181317_node_single_routed_tcp_large_native_sender_hwm128_probe_20260604.txt`
  - status: complete
  - throughput은 일부 셀에서 조금 더 높았지만 p95/p99 latency가 크게 나빠지고 RR 262144B는 floor 64보다 낮았다.
- 판정:
  - HWM floor 64와 sender native submit은 public contract 변경 없이 잔여 single failset을 줄이므로 유지한다.
  - HWM floor 128은 이득이 작고 tail latency와 queue 폭 비용이 커서 기본값으로 채택하지 않는다.
  - main 문서의 single residual count는 6개 그대로 두되, 해당 6개 셀의 비율과 결과 파일을 갱신한다.

## Node multi 잔여 cluster 재측정과 SPOT large active slot 16 채택

- 대상:
  - multi `MULTI_PUBSUB`
  - multi `MULTI_SPOT_REQREP`
  - multi `MULTI_SPOT_SENDSEND`
  - multi `MULTI_STREAM`
- 배경:
  - Node multi는 문서 기준 `미달 21/156 (13.5%)`로 10% gate를 넘고 있었다.
  - 잔여 cluster는 같은 multi script 계열이라, Python/Go/Rust에서 확인한 것처럼 active loop와
    current C baseline 재측정 효과를 먼저 분리해 확인했다.
  - public API 또는 benchmark-only native helper를 새로 노출하지 않고, perf client 내부 scheduling만
    바꾸는 후보를 우선했다.
- 변경:
  - `perf/multi/perf_multi_spot_reqrep_client.ts`와
    `perf/multi/perf_multi_spot_sendsend_client.ts`에서 131072B 이상 active slot 기본값을 8에서 16으로
    올렸다.
  - `PERF_MULTI_SPOT_REQREP_ACTIVE_SLOTS`, `PERF_MULTI_SPOT_SENDSEND_ACTIVE_SLOTS` env override는
    보조 실험용으로 남겼다.
  - 새 공개 API, public contract, C baseline, HWM/profile 기준은 바꾸지 않았다.
- 검증:
  - `npm run build`: 통과
- 측정:
  - `MULTI_PUBSUB tcp 65536/131072B` Node:
    `perf_node_multi_linux_20260604_193459_node_multi_pubsub_tcp_large_hwm2000_cli_probe_20260604.txt`
  - `MULTI_PUBSUB tcp 65536/131072B` C:
    `perf_c_multi_linux_20260604_193537_node_pubsub_tcp_large_c_recheck_20260604.txt`
  - `MULTI_PUBSUB ws 256B`, `wss 64B` Node:
    `perf_node_multi_linux_20260604_193723_node_multi_pubsub_ws256_wss64_recheck_20260604.txt`
  - `MULTI_PUBSUB ws 256B`, `wss 64B` C:
    `perf_c_multi_linux_20260604_193731_node_pubsub_ws256_wss64_c_recheck_20260604.txt`
  - `MULTI_SPOT_REQREP tcp/ws 131072B` Node 기본값 검증:
    `perf_node_multi_linux_20260604_195337_node_multi_spot_reqrep_active16_default_verify_20260604.txt`
  - `MULTI_SPOT_REQREP tcp/ws 131072B` C:
    `perf_c_multi_linux_20260604_193920_node_spot_reqrep_131072_c_recheck_20260604.txt`
  - `MULTI_SPOT_SENDSEND tcp/wss 256/65536/131072B` Node:
    `perf_node_multi_linux_20260604_194459_node_multi_spot_sendsend_residual_recheck_20260604.txt`
  - `MULTI_SPOT_SENDSEND tcp/wss 256/65536/131072B` C:
    `perf_c_multi_linux_20260604_194510_node_spot_sendsend_residual_c_recheck_20260604.txt`
  - `MULTI_SPOT_SENDSEND tcp 131072B` Node 기본값 검증:
    `perf_node_multi_linux_20260604_195408_node_multi_spot_sendsend_active16_default_verify_20260604.txt`
  - `MULTI_STREAM tcp/ws/wss/tls 64/256/1024B` Node:
    `perf_node_multi_linux_20260604_195155_node_multi_stream_small_recheck_20260604.txt`
  - `MULTI_STREAM tcp/ws/wss/tls 64/256/1024B` C:
    `perf_c_multi_linux_20260604_195416_node_multi_stream_small_c_recheck_20260604.txt`
- 결과:
  - `MULTI_PUBSUB tcp 131072B`: 32.5%로 통과.
  - `MULTI_PUBSUB ws 256B`: 34.0%로 통과.
  - `MULTI_PUBSUB wss 64B`: 31.7%로 통과.
  - `MULTI_SPOT_REQREP tcp 131072B`: 48.2%로 통과.
  - `MULTI_SPOT_REQREP ws 131072B`: 42.9%로 통과.
  - `MULTI_SPOT_SENDSEND tcp 65536B`: 33.2%로 통과.
  - `MULTI_SPOT_SENDSEND tcp 131072B`: 41.4%로 통과.
  - `MULTI_STREAM wss 256B`: 30.5%로 통과.
- 잔여:
  - `MULTI_PUBSUB tcp 65536B`는 22.1%로 여전히 미달이다.
  - `MULTI_SPOT_SENDSEND wss 256B`는 25.1%로 올랐지만 SPOT 기준에는 아직 못 닿는다.
  - `MULTI_STREAM` small은 `wss 256B`만 통과했고, 나머지 tcp/ws/wss/tls 64/256/1024B 조합은
    단순/stream 기준 30%에 못 닿는다.
- 판정:
  - SPOT large active slot 16은 public contract 변경 없이 두 SPOT large failset을 통과시키므로
    유지한다.
  - current C/Node 제한 재측정으로 통과한 셀은 main 문서 표에 반영한다.
  - Node multi 미달은 `21/156 (13.5%)`에서 `13/156 (8.3%)`로 줄어 10% gate 아래로 내려왔다.

## MULTI_SPOT_SENDSEND wss 256B active slot 16 후보 기각

- 대상:
  - multi `MULTI_SPOT_SENDSEND`
  - `wss`
  - `256B`
- 배경:
  - SPOT large에서 active slot 16이 통과를 만들었으므로, 같은 정책을 남은 WSS 256B에도
    적용할 수 있는지 확인했다.
  - 이 후보는 env override만 사용한 probe라 최종 코드 기본값은 바꾸지 않았다.
- 측정:
  - 명령: `PERF_MULTI_SPOT_SENDSEND_ACTIVE_SLOTS=16 ./perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT_SENDSEND --transports wss --msg-sizes 256 --duration 1 --runs 3 --results-tag node_multi_spot_sendsend_wss256_active16_probe_20260604`
  - Node: `perf_node_multi_linux_20260604_200031_node_multi_spot_sendsend_wss256_active16_probe_20260604.txt`
  - status: complete
- 결과:
  - median throughput은 12.670 Kops/s로, 현행 재측정 54.669 Kops/s보다 크게 낮다.
- 판정:
  - active slot 16은 131072B 이상 large 구간에만 유지한다.
  - small WSS 잔여 미달에는 적용하지 않는다.

## Node single routed tcp large current C 재측정과 잔여 1개 정리

- 대상:
  - single `DEALER_ROUTER`, `ROUTER_ROUTER`
  - `tcp`
  - `65536,131072,262144B`
- 배경:
  - 직전 표는 오래된 C full 기준으로 `DEALER_ROUTER`/`ROUTER_ROUTER` tcp 대용량 6개를
    모두 미달로 남겼다.
  - Node 쪽 sender native submit과 HWM floor 64는 유지했지만, 같은 현재 runtime에서 C 기준도
    함께 재측정해야 실제 잔여를 정확히 판단할 수 있다.
- 측정:
  - C 명령: `./perf/run_benchmarks.sh --reuse-build --pattern DEALER_ROUTER,ROUTER_ROUTER --transports tcp --msg-sizes 65536,131072,262144 --duration 2 --runs 3 --results-tag node_single_routed_tcp_large_c_recheck_20260604`
  - C report: `perf_c_single_linux_20260604_202935_node_single_routed_tcp_large_c_recheck_20260604.txt`
  - Node 기준 report: `perf_node_single_linux_20260604_181153_node_single_routed_tcp_large_native_sender_hwm64_20260604.txt`
  - `DEALER_ROUTER tcp 131072B` 5회 재확인:
    `perf_node_single_linux_20260604_203042_node_single_dr_tcp131072_confirm_20260604.txt`
- 결과:
  - `DEALER_ROUTER tcp 65536/131072/262144B`: 29.3/26.6/29.3%.
  - `ROUTER_ROUTER tcp 65536/131072/262144B`: 31.2/32.1/28.6%.
  - 6개 중 `DEALER_ROUTER tcp 131072B`만 routed 기준 28%에 못 닿는다.
  - `DEALER_ROUTER tcp 131072B` 5회 재확인 중앙값은 14.289 Kmsg/s로 current C 55.089 Kmsg/s 대비
    25.9%다.
- 추가 후보:
  - `PERF_SINGLE_ROUTED_LARGE_HWM_FLOOR=96`:
    `perf_node_single_linux_20260604_203109_node_single_dr_tcp131072_hwm96_probe_20260604.txt`
    는 12.188 Kmsg/s로 회귀했다.
  - `PERF_SINGLE_ROUTED_LARGE_HWM_FLOOR=192`:
    `perf_node_single_linux_20260604_203109_node_single_dr_tcp131072_hwm192_probe_20260604.txt`
    는 12.158 Kmsg/s로 회귀했다.
  - sender worker의 sequence를 `BigInt` 증가 대신 `number` 증가로 바꾸는 후보는
    `perf_node_single_linux_20260604_203306_node_single_dr_tcp131072_number_seq_retry_20260604.txt`
    에서 14.137 Kmsg/s로 현행보다 낮아 제거했다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
- 판정:
  - main 문서의 Node single은 `미달 6/144 (4.2%)`에서 `미달 1/144 (0.7%)`로 갱신한다.
  - 남은 single 미달은 `DEALER_ROUTER tcp 131072B` 하나다.
  - HWM 96/192와 number sequence 후보는 성능 개선 근거가 없어 코드에 남기지 않는다.

## Node multi stream echo direct result send 적용

- 대상:
  - multi `MULTI_STREAM`
  - `tcp, ws, wss, tls`
  - `64,256,1024B`
- 배경:
  - `MULTI_STREAM`은 Node server와 공용 C stream client 조합이다.
  - 잔여 small 미달은 server packet handler가 매 echo마다 public send operation builder를 만들고
    다시 submit하는 비용에 집중된다.
  - 새 native helper를 노출하지 않고, 이미 runtime binding surface에 있는
    `socketSendRoutingNoWaitResult`를 perf server 내부에서 직접 사용해 builder 생성만 줄였다.
- 변경:
  - `perf/multi/perf_multi_stream_server.ts`의 echo send 경로가 stream public packet handler와
    packet framing 의미는 유지하되, direct result send로 `DontWait` routed frame을 제출한다.
  - public `.d.ts`, 공개 stream API, C 기준, HWM/profile은 바꾸지 않았다.
- 측정:
  - Node current 재측정:
    `perf_node_multi_linux_20260604_204406_node_multi_stream_small_current_retry_20260604.txt`
  - Node 적용 후보:
    `perf_node_multi_linux_20260604_204847_node_multi_stream_direct_native_send_probe_20260604.txt`
  - C tcp/ws/wss 기준:
    `perf_c_multi_linux_20260604_204854_node_multi_stream_small_c_recheck_20260604.txt`
    (`tls` 3개는 partial이라 판정에서 제외)
  - C tls 기준:
    `perf_c_multi_linux_20260604_205009_node_multi_stream_tls_small_c_recheck_retry_20260604.txt`
  - server IO thread 8 기각 후보:
    `perf_node_multi_linux_20260604_205312_node_multi_stream_server_io8_probe_20260604.txt`
- 결과:
  - `MULTI_STREAM tcp 64B`: Node 87.954 Kops/s, C 291.148 Kops/s, 30.2%로 통과.
  - `MULTI_STREAM tcp 256B`: 29.7%로 기준에 근접했지만 아직 미달.
  - `MULTI_STREAM tcp 1024B`: 25.8%로 미달.
  - `ws`, `wss`, `tls` small은 direct result send 뒤에도 기준에 못 닿았다.
  - server IO thread 8은 tcp/tls 모두 direct result send 후보보다 낮아 코드에 반영하지 않는다.
- 검증:
  - `npm run build`: 통과
  - `node --test dist-tools/tests/optimization_guard.test.js`: 통과
  - `git diff -- bindings/node/dist/index.d.ts`: 변경 없음
- 판정:
  - `MULTI_STREAM tcp 64B` 1개가 추가 통과해 Node multi 미달은 `13/156 (8.3%)`에서
    `12/156 (7.7%)`로 줄었다.
  - 남은 multi 미달은 `MULTI_PUBSUB tcp 65536B`, `MULTI_SPOT_SENDSEND wss 256B`,
    `MULTI_STREAM` small 잔여 10개다.

## Node STREAM callback 소유권 후보 검토와 기각

- 대상:
  - multi `MULTI_STREAM`
  - `tcp, ws, wss, tls`
  - `64,256,1024B`
- 배경:
  - 이전 direct result send 적용 뒤에도 `MULTI_STREAM` small 잔여가 가장 많았다.
  - perf-only native echo export인 `socketStreamAttachPacketEcho`는 optimization guard가 금지하므로
    되살리지 않는다.
  - public stream handler 의미를 유지하는 범위에서 native callback이 JS Buffer를 만드는 소유권 경로만
    바꿔 볼 수 있는지 확인했다.
- 측정:
  - 변경 전 Node 제한 재측정:
    `perf_node_multi_linux_20260604_221304_node_stream_small_current_prepatch_20260604.txt`
    는 status=partial이었다. tcp 64B와 ws 256B가 실패했고, 실행 중 `malloc(): unaligned tcache chunk detected`,
    `double free or corruption (fasttop)`가 출력됐다.
  - C 제한 재측정:
    `perf_c_multi_linux_20260604_221940_node_stream_small_c_recheck_after_copy_20260604.txt`
    는 wss 3개가 실패해 status=partial이었다. tcp/tls/ws 결과만 참고했다.
  - JS Buffer copy 후보:
    `perf_node_multi_linux_20260604_221827_node_stream_small_copy_callback_20260604.txt`
    는 status=complete였다.
  - native message copy-owner 후보:
    `perf_node_multi_linux_20260604_222618_node_stream_small_msg_copy_owner_20260604.txt`
    는 tcp 64B 실패로 status=partial이었다.
- 결과:
  - JS Buffer copy 후보는 crash 없이 complete였지만, tcp 64/256/1024B가 66.814/67.040/62.528 Kops/s로
    current C tcp 기준 26.5/29.1/24.6%에 머물렀다.
  - ws/wss/tls small도 30% 기준에 닿지 못했다.
  - native message copy-owner 후보는 zero-copy 성격을 일부 유지했지만 complete가 아니었고,
    성공한 cell도 통과권 개선을 만들지 못했다.
- 검증:
  - 후보 적용 중 `npm run rebuild-native`: 통과
  - `node dist-tools/tests/optimization_guard.test.js`: 통과
  - `node dist-tools/tests/socket_surface.test.js`: 통과
- 판정:
  - 두 후보 모두 잔여 미달을 줄이지 못했다.
  - copy 후보는 안정성은 나아졌지만 성능 표를 개선하지 못하고, copy-owner 후보는 partial이라 최종 근거가
    될 수 없다.
  - 최종 코드에는 남기지 않고, `MULTI_STREAM` small 잔여는 public stream handler 경계 비용으로 유지한다.

## Node multi current 제한 재측정과 stream native frame 후보 기각

- 대상:
  - single `DEALER_ROUTER tcp 131072B`
  - multi `MULTI_STREAM` small
  - multi `MULTI_PUBSUB tcp 65536B`
  - multi `MULTI_SPOT_SENDSEND wss 256B`
- 배경:
  - Node single 잔여 1개와 multi 잔여 cluster를 current C 기준으로 다시 확인했다.
  - `MULTI_STREAM`은 JS에서 frame Buffer를 만들고 native result-send helper를 호출하는 경계가 남아 있어,
    packet frame 조립을 internal native helper로 내리면 줄어드는지 확인했다.
- 측정:
  - `DEALER_ROUTER tcp 131072B` Node:
    `perf_node_single_linux_20260604_223147_node_single_dr_tcp131072_current_recheck_20260604.txt`
  - `DEALER_ROUTER tcp 131072B` C:
    `perf_c_single_linux_20260604_223154_node_single_dr_tcp131072_c_current_recheck_20260604.txt`
  - `DEALER_ROUTER tcp 131072B` HWM 8 probe:
    `perf_node_single_linux_20260604_223229_node_single_dr_tcp131072_hwm8_probe_20260604.txt`
  - `MULTI_STREAM` current 재측정:
    `perf_node_multi_linux_20260604_223654_node_multi_stream_small_current_recheck2_20260604.txt`
    는 ws 64B 실패로 status=partial이었다.
  - `MULTI_STREAM` native frame 후보:
    `perf_node_multi_linux_20260604_224346_node_multi_stream_native_frame_probe_20260604.txt`
    는 tcp 64B, tcp 1024B, ws 1024B 실패로 status=partial이었다. 실행 중
    `double free or corruption (fasttop)`가 출력됐다.
  - 안정 잔여 current Node:
    `perf_node_multi_linux_20260604_224911_node_multi_pubsub_spot_residual_current_recheck_20260604.txt`
    는 status=complete였다.
  - 안정 잔여 current C:
    `perf_c_multi_linux_20260604_224926_node_multi_pubsub_spot_residual_c_recheck_20260604.txt`
    는 status=complete였다.
- 결과:
  - `DEALER_ROUTER tcp 131072B`는 Node 14.138 Kmsg/s, C 53.012 Kmsg/s로 26.7%에 그쳤다.
    HWM 8 probe도 effective HWM이 64로 유지됐고 median 13.972 Kmsg/s라 개선되지 않았다.
  - `MULTI_STREAM` native frame 후보는 complete가 아니고 corruption 로그가 있어 최종 코드에 남기지 않았다.
  - `MULTI_PUBSUB tcp 65536B`는 Node 43.032 Kmsg/s, C 146.725 Kmsg/s로 29.3%가 되어 통과했다.
  - `MULTI_SPOT_SENDSEND wss 256B`는 Node 47.415 Kops/s, C 167.631 Kops/s로 28.3%가 되어 통과했다.
- 검증:
  - native frame 후보 적용 중 `npm --prefix bindings/node run rebuild-native`: 통과
  - `npm --prefix bindings/node run build`: 통과
  - `node bindings/node/dist-tools/tests/optimization_guard.test.js`: 통과
  - `node bindings/node/dist-tools/tests/socket_surface.test.js`: 통과
  - 후보 제거 뒤에도 같은 build/guard/socket-surface 검증을 다시 통과했다.
- 판정:
  - 이번 라운드에서 Node multi 미달은 `12/156 (7.7%)`에서 `10/156 (6.4%)`로 줄었다.
  - 남은 Node multi 미달은 `MULTI_STREAM` small 10개다.
  - public stream handler 경계를 유지하면서 시도한 native frame 후보는 안정성 문제 때문에 기각한다.

## Node MULTI_STREAM completion wait 보강

- 대상:
  - multi `MULTI_STREAM` small
- 배경:
  - Python multi에서 shared C `perf_stream_client`의 completion wait 보강으로 stream RESULT 누락이
    해소됐다.
  - Node도 같은 shared C client를 쓰지만 completion wait를 전달하지 않아 active window 뒤의
    in-flight reply를 충분히 수집하지 못할 수 있었다.
- 변경:
  - `perf/multi/perf_multi_orchestrator.ts`에서 `MULTI_STREAM` client spawn에
    `--completion-wait-ms`를 전달한다.
  - 기본값은 `2000` ms다. `PERF_MULTI_STREAM_COMPLETION_WAIT_MS` 또는
    `PERF_STREAM_COMPLETION_WAIT_MS`로 조정할 수 있다.
- 측정:
  - 현재 Node stream small:
    `perf_node_multi_linux_20260604_235751_node_multi_stream_small_current_20260604_retry.txt`
    는 status=complete였다.
  - C 기준:
    `perf_c_multi_linux_20260605_000005_node_multi_stream_small_c_compare_20260604.txt`
    는 status=complete였다.
  - Node 2000ms completion wait:
    `perf_node_multi_linux_20260605_001148_node_multi_stream_completion_wait_2000_small_20260604.txt`
    는 status=complete였다.
  - 2000ms runs=3 tcp 보강:
    `perf_node_multi_linux_20260605_000818_node_multi_stream_completion_wait_2000_tcp_probe_20260604.txt`
    는 status=complete였다.
  - 3000ms tcp 1024B 단독 보강:
    `perf_node_multi_linux_20260605_000849_node_multi_stream_completion_wait_3000_tcp1024_probe_20260604.txt`
    는 status=complete였다.
- 결과:
  - 2000ms complete small 재측정 기준으로 `tcp 64/256/1024B`는 37.2/47.9/37.4%,
    `tls 64/256/1024B`는 33.8/31.9/43.9%, `wss 64/256/1024B`는 35.9/35.9/37.8%로
    통과권에 들어왔다.
  - `ws 64/256/1024B`는 26.8/24.8/25.6%로 여전히 미달한다.
- 기각 후보:
  - stream server idle wait 50ms를 1ms로 낮춘 후보는 `tcp 1024B` 단독 runs=3 median이
    C 대비 약 25.8%에 그쳐 통과를 만들지 못해 되돌렸다.
  - raw `socketStreamAttach` callback에서 `Message` materialization을 건너뛰는 후보는
    `malloc_consolidate()`/`double free or corruption` 로그와 partial report가 나와 되돌렸다.
  - 3000ms와 10000ms completion wait는 일부 multi-size run에서 client 메모리 오류로 partial이
    되어 기본값으로 채택하지 않았다.
- 검증:
  - `npm --prefix bindings/node run build`: 통과
  - `node bindings/node/dist-tools/tests/optimization_guard.test.js`: 통과
- 판정:
  - Node multi 미달은 `10/156 (6.4%)`에서 `3/156 (1.9%)`로 줄었다.
  - 남은 Node multi 미달은 `MULTI_STREAM ws 64/256/1024B` 세 개다.

## Node single routed metric native 수신 보강

- 대상:
  - single `DEALER_ROUTER tcp 131072B`
- 배경:
  - 2026-06-04 current 재측정에서는 Node 14.138 Kmsg/s, C 53.012 Kmsg/s로 26.7%에
    그쳐 마지막 single 미달로 남았다.
  - HWM 8, HWM 16, 512KB socket buffer probe는 통과권 개선을 만들지 못했다.
  - 남은 비용은 routed 수신마다 128KB payload Buffer를 JS로 materialize한 뒤 metric header만
    읽는 부분으로 좁혀졌다.
- 변경:
  - native addon에 perf 전용 `routerRecvSingleMetricLatency`를 추가했다.
  - 이 helper는 공개 수신 API를 바꾸지 않고, single-part routed message에서 stop token,
    metric header, active window를 native에서 확인한 뒤 latency ns 숫자만 JS collector에 넘긴다.
  - JS collector에는 이미 검증된 header를 다시 Buffer로 읽지 않는 `recordLatencyNs(...)` 경로를
    추가했다.
- 측정:
  - 변경 전 Node 5회 current:
    `perf_node_single_linux_20260605_011611_node_single_dr_tcp131072_current_recheck_20260605.txt`
    는 status=complete였고 median 13,921.8 msg/s였다.
  - 변경 전 C 5회 current:
    `perf_c_single_linux_20260605_011536_node_single_dr_tcp131072_c_current_recheck_20260605.txt`
    는 status=complete였고 median 53,845.8 msg/s였다.
  - 변경 후 Node 5회:
    `perf_node_single_linux_20260605_012112_node_single_dr_tcp131072_native_metric_20260605.txt`
    는 status=complete였고 median 22,006.6 msg/s였다.
  - `ROUTER_ROUTER tcp 131072B` guard:
    `perf_node_single_linux_20260605_012100_node_single_rr_tcp131072_native_metric_guard_20260605.txt`
    는 status=complete였다.
- 결과:
  - `DEALER_ROUTER tcp 131072B`는 current C 대비 40.9%로 통과권에 들어왔다.
  - `ROUTER_ROUTER tcp 131072B`도 complete guard 기준으로 기존 통과권을 유지했다.
- 검증:
  - `npm --prefix bindings/node run build`: 통과
  - `npm --prefix bindings/node run typecheck`: 통과
  - `npm --prefix bindings/node run rebuild-native`: 통과
  - `node bindings/node/dist-tools/tests/optimization_guard.test.js bindings/node/dist-tools/tests/dealer_router.test.js`: 통과
- 판정:
  - Node single 미달은 `1/144 (0.7%)`에서 `0/144`로 줄었다.
  - Node single과 multi 모두 남은 미달이 없다.
