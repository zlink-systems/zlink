[English](./framework-node-0.29.1.md) | [한국어](./framework-node-0.29.1.ko.md)

# ZLink Node.js Framework 0.29.1 릴리스 노트

Framework 0.29.1은 Node.js binding 1.17.0과 Core 1.17.0을 사용합니다.

## 변경

- `StreamClient` tutorial의 `framework-codec-protobuf` pin을 Framework 버전에 맞추고, README 링크를 export된 mirror에서도 열리는 절대 URL로 바꿨습니다. (#1562)

## 결함 수정

- 같은 Instance Spot을 깨우는 operation이 활성화 도중에 도착하면 `Unavailable`·`stale_target`으로 실패하던 0.29.0 회귀를 수정했습니다. 뒤에 도착한 operation은 진행 중인 활성화에 합류하고, Ready 뒤 도착 순서대로 처리됩니다. (#1571)
- Owner node가 강제 종료된 Actor를 다시 `Create`·`GetOrCreate`하면 `location owner lease is unavailable`로 계속 실패하던 결함을 수정했습니다. Factory 등록에서 relocation을 끈(`DisableRelocation`) Actor type은 owner lease가 끝난 기존 record를 해제하고 새 incarnation을 만듭니다. Relocation 정책이 켜진 type은 계속 `Unavailable`을 반환합니다. (#1570)
- Actor 종료가 이미 수락한 handler turn이 끝나기 전에 dependency 정리를 시작하던 결함을 수정했습니다. (#1579)

## 설치

```bash
npm install @zlink-systems/framework@0.29.1 @zlink-systems/http-client@0.29.1
```

릴리스 태그는 [`framework-node/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.29.1)입니다.
[한국어](./framework-node-0.29.1.ko.md) | [English](./framework-node-0.29.1.md)
