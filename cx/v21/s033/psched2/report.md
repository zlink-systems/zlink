Node busy-window 검사는 handler A와 뒤따른 128건, 합계 129건 동안 worker CV의 notify_one 호출 수를 worker_notifications_for_129_records로 출력한다. queued handler thread와 A의 thread를 합친 distinct thread 수를 workers_participating으로 출력한다. A가 한 worker를 점유하므로 통지 상한은 max_worker_count - 1이고 참여 worker 상한은 max_worker_count다. 이 상한은 offload_executor.cpp:177-217의 reserve_workers_locked가 reserved worker를 먼저 빼고 idle worker만 예약하는 규칙에서 따른다. S02 §2의 독립 진행 검사도 유지한다.

Steady-stream 검사는 같은 Node에 1,000건을 50us 간격으로 발행하고 handler마다 500us 지연을 둔다. worker_notifications/1,000과 distinct_workers/1,000을 notifications_per_record와 workers_per_record로 출력한다. W=4에서 통지 수와 distinct worker 수 각각 4 이하를 assert한다. 예약된 worker 수를 반영하고 남은 수요만 idle worker에 예약하는 규칙은 offload_executor.cpp:189-215에 있다. stream-wakes 음성 대조는 발행마다 추가 notify_one을 호출해 동일한 oracle의 통지 상한을 넘으면 exit 1로 끝난다. 기존 음성 대조 이름과 동작은 변경하지 않았다.

Core verdict.md가 없어 빌드와 테스트는 실행하지 않았다. 출력될 두 측정값은 아직 미측정이다. bash scripts/format/format.sh --check cpp는 통과했다. Diff는 change.diff에 저장했다.
