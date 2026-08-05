# Java SpotService E2E

이 디렉터리는 공통 E2E Config 2 SpotService 시나리오를 Java framework public API로 검증한다.
기존 단일 application 구현은 `.NET` 기준 역할 분리에 맞춰 Gradle subproject로 나누었다.

## 역할

- `Shared`: 기존 Java SpotService 구현의 공통 contract, spot, actor, handler, evidence, timer, stream support 타입.
- `Server/Gateway`: HTTP scenario endpoint와 framework gateway process. Client가 요청한 scenario mode를
  public framework 경로로 실행한다.
- `Server/Play`: play MeshNode process. 하나의 ROUTER에서 ChannelName·Spot·Actor를 제공하고 STREAM endpoint는 별도 stream node가 호스팅한다.
- `Server/MultiNode`: `.NET` 기준 source role에 맞춘 multi-node role project. 현재 Java 구현은 shared
  play configuration을 사용하고, 고급 multi-node scenario는 feature-map gap으로 남긴다.
- `Server/Session`: `.NET` 기준 source role에 맞춘 session role project. 현재 Java 구현은 shared play
  configuration을 사용하고, remote session scenario는 feature-map gap으로 남긴다.
- `Server/Publisher`: `ZLinkSpotPublisherClient` publish scenario process.
- `Client`: HTTP driver process. framework runtime으로 뜨지 않고 `Server/Gateway`의 scenario endpoint를 호출한다.

## 실행

```bash
./run_e2e.sh
```

runner는 gateway, play, publisher, client role별 installDist binary를 직접 실행한다. gateway, play,
publisher role은 같은 Redis location store endpoint와 실행별 key prefix를 공유한다. 실행 로그와
evidence는 `logs/<run-id>/` 아래에 남는다.

완료/gap 분류는 `feature-map.ko.md`를 기준으로 본다. 공통 E2E나 다른 언어 구현만 근거로 Java public
API를 새로 추가하지 않는다.
