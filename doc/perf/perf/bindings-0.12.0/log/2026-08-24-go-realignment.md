# Go 바인딩 0.13.0 재정렬 (2026-08-24)

구현은 재정렬 에이전트가 수행했고(감독자가 오판으로 중단했으나 작업분은
트리에 온전히 보존), 검증은 감독자가 직접 완료했다.

## 변경
- send_ready/routed admission 네이티브 기계장치 삭제
  (socket_send_ready.go, routed_admission.go 및 배선)
- send/routed send = 동기 `Submit(ctx)` — Core blocking send 래핑
  (goroutine 블로킹이 Go의 관용적 대기, ctx가 취소/시한 소유)
- publish 동기 유지, request는 Core reply callback → 완료 채널
- vendored 헤더 core/include와 byte-identical 재동기화
- raw-core allowlist(coreVersion/sha256/publicSymbols) 재생성
- perf 하네스 신 계약 정렬

## 검증 (감독자 직접)
- go build ./... : 통과 / go test ./... : 전체 통과
- grep: send_ready 0건, 헤더 diff 0건
- 스모크(release 0.13.0): single 전 패턴 `status: complete`;
  multi 94/97 — 실패 2셀(PUBSUB/tls 64B, STREAM/tcp 65536B)은
  write-turn 수정 이전 asset 기준이라 최종 스윕에서 재판정
