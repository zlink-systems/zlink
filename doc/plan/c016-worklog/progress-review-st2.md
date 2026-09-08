# review-ST-2 진행

- 대상: `/home/hep7hep7/project/zlink-work/st1` 미커밋 diff와 신규 STREAM 테스트. 읽기·리뷰 전용이며 이 진행 파일과 리뷰 보고서만 작성한다.
- 공통 규칙, review-ST-1, ST-2 보고서와 소유권 코드 확인 완료. 1차 B-1의 command 해제 뒤 지연 fallback 순서는 재검증으로 차단된다.
- blocking recv는 lease 해제 뒤 대기한다. command waiter 등록·broadcast는 같은 sync에서 연결되며, 실제 command 후보는 바깥 command_owner_sync에서 직렬화된다.
- 추가 검토: async→available 해제가 receive.sync 없이 이루어지는 경계, progress_epoch 비원자 접근, Boost.Asio 테스트의 socket/io_context 수명 및 실패 정리, hotpath poller 호출 빈도.
- 빌드·테스트·벤치마크·커밋은 실행하지 않았다.
- 추가 확인: async 해제는 기존 mutex-only entrant가 끝나기 전에 available을 발행할 수 있다. command 전용 상태 재검증은 맞지만 전체 프로토콜의 배타 증명은 미완료다.
- TSan 원시 로그에서 receive_once_guarded와 async 종료 notify의 epoch race, 9 warnings 및 shutdown 기존 FAIL을 확인했다. baseline 로그들은 빈 파일이므로 3/3 종료 코드는 보고서 기록 이상의 독립 증거가 없다.
- 새 테스트의 socket은 client별 지역 io_context보다 오래 남는다. Boost.Asio Windows backend의 destroy는 닫힌 socket에도 service mutex를 사용한다. 실패 경로 close/read 동시 호출도 API thread-safety 조건을 어긴다.
- hotpath_bench의 stream_tcp·router_router_tcp는 blocking recv를 사용하며 public POLLIN poller를 사용하지 않는다. has_in 비용은 실제 poller workload의 별도 회귀 후보다.
- 리뷰 완료: `review-st2.md` 작성. 차단 4건(async 해제 배타 공백, epoch data race, 테스트 io_context/socket 수명 역전, 실패 경로 동시 close). 1차 command fallback 반례와 일반 recv의 대기 전 lease 해제는 해소로 판정했다.
- 대상 diff는 마지막 확인에서도 tracked 5개 파일 294 insertions/13 deletions와 신규 테스트 1개였다. 소스·스펙·테스트와 빌드 산출물은 수정하지 않았고 실행 검증·커밋도 하지 않았다.
