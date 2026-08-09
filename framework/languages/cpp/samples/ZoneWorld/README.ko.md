# ZoneWorld C++ sample

이 sample은 네 zone에서 player Actor가 이동하는 흐름과 Gateway의 bound STREAM session,
ZoneNode 두 process 사이의 relocation, 인접 zone Logical Multicast, Ops fanout과 점검 상태를
검증한다. Application은 좌표와 zone 규칙을 소유하고, actor owner와 transport RID는 Framework가
결정한다.

Linux 또는 WSL에서 다음 명령을 실행한다.

```bash
./framework/languages/cpp/samples/ZoneWorld/run_sample.sh
```

runner는 전용 Redis container와 실행별 key prefix를 만들고 ZoneNode 두 개, Ops, Gateway와
headless client를 별도 process로 실행한다. client는 입장, 같은 zone 이동, OutOfRange·TooFar 거부,
zone 변경, relocation 직후 request payload, announce와 점검 상태를 확인한다. runner는 이어서
deterministic bot 생성과 이동 evidence를 확인한다. 성공하면 다음
marker를 출력하고 모든 process와 Redis container를 정리한다.

```text
zoneworld=completed
PASS ZoneWorld.Cpp
zoneworld sample result=passed
```
