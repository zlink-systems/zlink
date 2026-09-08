# review-SD-4 진행

- 상태: 완료
- 기준: base `84d25131a6`, artifact `~/project/zlink-work/all-artifacts/SD-4.patch`
- 수행 제한: 소스·스펙 수정, 빌드, 테스트, benchmark, commit 없이 정적 리뷰와 기존 로그 확인만 수행
- 확인 완료: 공통 규칙, `review-sd2.md`, `core-rf-SD-4-report.md`, patch·post-patch source, ZMP §8/§9, Beast `async_write`, TSan/hotpath/WS gate 기존 로그
- 통과 확인: SD-2 message-boundary 기본 gather 원복, TCP env opt-in·RAW 비활성, body pointer 수명과 completion 반환 순서, max 초과 fallback, WS/WSS 전용 admission, 새 production 상태·옵션·env 0, B2·D-f·계측 app의 SD-3 이후 불변, public header·version script 불변
- 차단 후보: 128 KiB target에서 첫 64 KiB frame도 target-crossing 여부 없이 pointer gather로 batch를 닫아 뒤의 small ready frame을 별도 write로 미룬다. 첫 회귀 unit은 순서를 `small → 64 KiB`로 뒤집어 원래 `64 KiB → small` 반례를 고정하지 않는다.
- 증거 한계: TSan 27/27·sanitizer 0은 `sd1/core/build-tsan/Testing/Temporary/LastTest.log`에서 확인했으나 artifact 디렉터리에 독립 보존 로그가 없다. hotpath는 5셀 중 1셀(`dealer_router_reqrep_inproc`, 0.9496) 실패다.
- 최종 판정: 차단 1건. `B-SD4-1` 때문에 `B-SD2-1`은 미해소이며 현재 artifact는 채택 불가다.
- 산출물: `doc/plan/c016-worklog/review-sd4.md`
