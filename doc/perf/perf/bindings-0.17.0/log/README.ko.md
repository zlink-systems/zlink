# log

실행 명령, 후보 검토, 프로파일, 구현 변경 과정을 날짜·언어별 파일로 기록한다(계획서에는 결과만 남긴다).
- [2026-09-05-cpp-multi-ws-wss-reqrep-remeasure.ko.md](2026-09-05-cpp-multi-ws-wss-reqrep-remeasure.ko.md) — C runner ws/wss 제출 턴 수정(D-B89) 뒤 C++ Multi `ws`·`wss` REQREP 4셀 C·C++ 재측정(`보류(C 기준 이상)` 해소)
- [2026-09-05-cpp-multi-tcp-pubsub-pass1.ko.md](2026-09-05-cpp-multi-tcp-pubsub-pass1.ko.md) — C++ Multi `tcp` `MULTI_PUBSUB` 자체 hot-path pass 1(no-go)과 3-run 재짝지음 판정 `미달(81.5%)`
- [2026-09-05-cpp-multi-tls-ws-wss-dd-r3.ko.md](2026-09-05-cpp-multi-tls-ws-wss-dd-r3.ko.md) — C++ Multi `tls`·`ws`·`wss` `MULTI_DEALER_DEALER` 3-run 재짝지음, `보류` 79.3/85.3/91.1%, DD latency metric bimodal 주의
- [2026-09-05-cpp-single-before.ko.md](2026-09-05-cpp-single-before.ko.md) — C++ Single suite 7 pattern × 6 transport paired before(one-way latency=큐 깊이 해석, 수신 경로 pass 1 착수)
- [2026-09-05-cpp-single-recv-pass1.ko.md](2026-09-05-cpp-single-recv-pass1.ko.md) — C++ Single 수신 경로 pass 1(library no-go, 러너 getenv 버그 수정) + one-way 5 pattern 재짝지음
