# Java ladder 재측정 진행

조건: 고정 빌드 ee7e5c3343, C2·C3 off, SCHED_OTHER, source 0–5 / target 8–13, warmup 10초·active 5초. Binding은 [java-2026-11-13b](../binding-daily/java-2026-11-13b.json) 사용. 매 구현·셀 전환마다 warm 1회 후 counted 2회; L4와 L5~L7을 번갈아 측정한다. [호출 형태](call-shape.md).
