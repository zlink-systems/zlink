# Node Framework Performance Plan

> 공통 정책: [ZLink Framework Performance Policy](../README.ko.md)
>
> 적용 범위: `framework/languages/node`

## 1. 목적

Node framework perf는 event loop, native addon, TypeScript/JavaScript wrapper 비용이 섞인다.
따라서 runner는 native addon build/runtime path와 event loop scheduling 조건을 report에
명확히 남긴다.

## 2. 측정 원칙

- Node framework public API만 사용한다.
- native addon 내부 helper를 benchmark hot path에서 직접 호출하지 않는다.
- generated `dist` 또는 `dist-tools` 산출물이 source와 불일치하면 실패한다.
- event loop turn을 progress 보정용 timer로 숨기지 않는다.
- callback/event-loop 비용은 Node framework 사용자 경로의 일부로 기록한다.

## 3. Scenario 적용

우선순위:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `stream_send`
5. `spot_to_spot_request_reply`
6. `route_mesh_request_reply`
7. `http_handler_roundtrip`

Node에서 아직 framework 구현이 없는 scenario는 `unsupported`로 둔다. 같은 이름으로 binding
perf나 raw native addon perf를 대신 실행하지 않는다.
우선순위 목록은 구현 순서일 뿐이다. 최종 report는 공통 정책의 모든 scenario를
`complete` 또는 `unsupported`로 기록해야 한다.

## 4. Node Metadata

```json
{
  "node_version": "...",
  "v8_version": "...",
  "native_addon_path": "...",
  "typescript_build": "dist",
  "event_loop_policy": "default",
  "npm_script": "..."
}
```

## 5. Runner 위치

권장 위치:

```text
framework/languages/node/perf/run_benchmarks.sh
framework/languages/node/perf/results/
```

tracked JavaScript 산출물이 있는 구조라면 TypeScript source와 tracked JS를 함께 갱신한다.

## 6. 금지 사항

- native addon direct API를 호출해 framework 결과로 보고하지 않는다.
- `setInterval`, 짧은 sleep, busy loop를 completion progress 소유자로 두지 않는다.
- stale native addon이나 stale generated JS로 측정하지 않는다.
- event loop delay를 임의로 보정해 latency에서 빼지 않는다.

## 7. 초기 구현 순서

1. 4KB smoke runner와 공통 JSON report writer를 만든다.
2. client-server request/reply와 send부터 연결한다.
3. stream과 spot scenario를 추가한다.
4. native addon path와 generated artifact 검증을 runner에 넣는다.
5. full payload matrix를 추가한다.
6. 동시성 프로파일 `serial`, `pipelined`, `concurrent`를 추가한다.
