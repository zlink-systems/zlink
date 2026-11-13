테스트 fixture에서 responder는 기존 테스트 대상 ZLinkJavaRawServicePort에 두고 requester socket은 같은 Context를 공유하는 별도 ZLinkJavaRawServicePort에 두었다. 따라서 port 또는 runtime close는 requester를 닫지 않는다. reply 결과를 모두 확인한 다음 requester port를 닫아 미완료 요청이 5초 timeout까지 기다리지 않게 했다. 허용 reply 결과와 jdr5가 추가한 진단 실패 메시지는 유지했다. 두 race 테스트의 최종 반복 횟수는 각각 100이다.

stress-ng 부하는 jdr5와 같은 `stress-ng --cpu 16 --cpu-method loop --metrics-brief` 설정으로 실행했고 5분 1.56초 뒤 SIGTERM으로 종료했다. 부하 로그는 `/root/jobs/fx/cx/v21/s033/jdr6/load.log`이다. 관측한 MemAvailable 최저값은 15.9 GiB로 12 GiB 기준을 지켰다.

부하 중 port close race는 1000회 중 984회 통과하고 16회 실패했다. 실패는 NOT_FOUND/ENOENT 9회와 INVALID_HANDLE 7회였으며 close 자체의 결과는 모두 성공했다. 로그는 `/root/jobs/fx/cx/v21/s033/jdr6/race-port-1000.log`이고 JUnit 원본은 `framework/languages/java/zlink-framework-core/build/test-results/test/TEST-systems.zlink.framework.runtime.binding.ZLinkJavaRawServicePortContractTest.xml`이다.

부하 중 runtime close race는 1000회 중 979회 통과하고 21회 실패했다. 실패는 NOT_FOUND/ENOENT 17회와 INVALID_HANDLE 4회였으며 close 자체의 결과는 모두 성공했다. 로그는 `/root/jobs/fx/cx/v21/s033/jdr6/race-runtime-1000.log`이고 같은 JUnit XML 경로에 결과가 기록됐다.

요청한 전체 Gradle 명령은 unit test 20,046개 중 1개 실패로 종료했다. 실패는 `repliesRacingPortCloseAreRejectedAfterCloseStartsAndCloseSucceeds` 반복 5의 reply 결과 assertion이다. Gradle은 실패 때문에 integrationTest를 실행하지 않았다. 로그는 `/root/jobs/fx/cx/v21/s033/jdr6/gradle-full.log`이다.

`/root/jobs/fx/cx/v21/s033/jdr6/format-check-final.log`에 기록한 Java format check는 통과했다. 최종 `git diff --check`는 measure-shared.sh를 통해 실행해 통과했다. 결과 로그는 `/root/jobs/fx/cx/v21/s033/jdr6/diff-check-final.log`이며 출력은 비어 있다.

전체 미커밋 diff는 `/root/jobs/fx/cx/v21/s033/jdr6/change.diff`에 저장했다. 변경 파일은 Java port와 socket options의 기존 미커밋 변경 및 `ZLinkJavaRawServicePortContractTest.java`이며, 이번 fixture 변경은 Java test source에만 적용했다.
