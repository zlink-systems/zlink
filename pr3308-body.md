Closes #3308

## 원인
`StreamSessionForcedCleanupTests`의 두 시험(`Stream_Request_Emits_Received_Dispatched_Then_Replied_With_Wire_Correlation` 외 1건, `:496-504`·`:586-591`)이 `ReplyCompleted`·전송 frame 완료만 기다린 뒤, 비동기 telemetry 전달(#3180·#3246)의 기록 완료를 기다리지 않고 message-flow 기록 목록을 복사했습니다. 첫 실패: `Expected: 3, Actual: 0`. 분류: 시험 동기화 결함, 제품 코드 변경 없음.

## 수정
- 단언 전에 기존 `WaitUntilAsync`로 기록 3건 도착을 기다립니다. 기록 수·순서·correlation 단언은 그대로입니다.
- 시험 logger가 provider thread에서 기록하므로 `List<string>`을 `ConcurrentQueue<string>`으로 바꿨습니다.

## 검증
- 부하(`taskset -c 8-15`, `stress-ng --cpu 8`) 50회: 수정 전 2/50 실패 → 수정 후 0/50, fixture 전체 37/37

### 로컬 .NET 시험 (pre-pr.sh, HEAD 41a808cf0a, main efb486bc98 병합)
| 구성 | Unit | Contract | Stream | 결과 |
|---|---|---|---|---|
| Debug net8.0 | 4145 | 98 | 269 | 통과 |
| Release net10.0 | 4143 | 98 | 269 | 통과 |
- Debug·Release 빌드, format.sh --check dotnet 통과

🤖 Generated with [Claude Code](https://claude.com/claude-code)
