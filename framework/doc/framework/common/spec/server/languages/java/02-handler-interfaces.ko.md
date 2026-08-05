# Java handler interface 문서 위치

Java handler와 application-facing public signature는 [exact interface 목차](interfaces/README.ko.md)에서
기능별로 제공한다.

- [Channel messaging](interfaces/channel-messaging.ko.md)
- [Spot](interfaces/spots.ko.md)
- [Actor](interfaces/actors.ko.md)
- [STREAM session](interfaces/stream-session.ko.md)
- [Monitoring](interfaces/monitoring.ko.md)

Actor와 Instance Spot의 relocation 동작은 factory configure callback에 직접 연결하며 별도 relocation registry를 두지 않는다.
