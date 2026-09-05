2026-09-05 KST START: detached main 기준 상태 확인; PUBSUB C/C++ parity 조사 시작.
2026-09-05 KST EDIT: server START 후 HWM 재적용·재계산·detail 출력, client 빈 filter·shape/deadline/poll parity 반영.
2026-09-05 KST VERIFY: 지정 1회 benchmark complete(2/2), 그러나 server HWM 64=4MiB·4096=1MiB로 C 기준과 반대; 재측정 없이 원인 조사.
2026-09-05 KST FINAL: PUBSUB targets 재빌드·diff-check 통과; HWM lifecycle blocker와 비교표를 summary에 기록.
EXIT:1
