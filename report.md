# C++ ChannelHeader `string_view` 후보 2차 측정 보고서

## 결과

실험 목적으로 Editions 2023 임시 proto에 C++ `string_type = VIEW`를 적용했다. C++에서 golden 13개가 통과했고 Node, Java, C# 생성물도 같은 wire bytes를 확인했다. 저장소의 proto, runtime 코드, generator 버전 pin은 원상 복구했다.

VIEW는 기존 protobuf와 비교해 encode 시간이 비슷했지만 decode는 request 12.0%, reply 12.9% 느렸다. 문자열을 borrowed view로 읽은 뒤 Framework header가 소유권을 가져야 해서 값 복사와 metadata map 구성이 남는다. 이 측정 결과만으로 VIEW 후보를 채택할 근거는 없다.

## C++ 측정

Release preset, source binding, protobuf 6.33.4로 빌드했다. 컴파일은 `cpp-build-slot.sh`와 `--parallel 3`을 사용했고 최종 build 디렉터리는 1.5 GB였다가 제거했다. 각 프로세스는 200,000회 반복했다. VIEW와 protobuf는 `/root/jobs/fx/measure.lock`의 exclusive lock 안에서 CPU 8–13에 고정해 번갈아 5회 측정했다. encode는 goldens의 full request/error response bytes가 정확히 일치하는지 먼저 확인했다.

중앙값, ns/op:

| 연산 | JSON 기준 `8654ef0765`¹ | protobuf `ddbad97c76`² | protobuf VIEW² |
|---|---:|---:|---:|
| request encode | 95.855 | 90.048 | 90.430 |
| request decode | 423.055 | 904.330 | 1012.620 |
| reply encode | 73.444 | 67.976 | 65.879 |
| reply decode | 202.593 | 467.429 | 527.741 |

¹ JSON 수치는 앞 단계와 동일한 micro의 5회 교차 측정에서 가져왔다(`/root/jobs/fx/cx/v21/s034/s1impl/cpp/raw/micro.log` 마지막 JSON 5행). ² 현재 protobuf와 VIEW 수치는 이번 작업에서 같은 Release 빌드로 새로 번갈아 측정했다(`/root/jobs/fx/cx/v21/s034/s2view/raw/alternating-micro.log`).

같은 실행에서 VIEW/protobuf의 encode 차이는 request +0.4%, reply −3.1%였다. decode는 request +12.0%, reply +12.9%였다. 이전 campaign의 protobuf 중앙값은 request 84.903/790.519 ns, reply 63.475/357.003 ns였다. 재측정한 현재 protobuf 수치가 이전 수치보다 높으므로, 후보 효과 판정은 동일 실행에서 번갈아 얻은 protobuf/VIEW 쌍을 우선한다.

### VIEW decode 단계

각 stage를 200,000회 계측한 뒤 5개 실행의 중앙값을 냈다. header 구성은 validator 전후의 필드 설정, VIEW에서 소유 문자열로 복사, metadata map 삽입을 합친 값이다. 계측 자체의 시계 읽기 비용이 포함된다.

| 입력 | `ParseFromArray` (ns) | header 구성 (ns) | validator (ns) | decode 전체 (ns) |
|---|---:|---:|---:|---:|
| full request, 132 B | 398 | 238 | 114 | 1012.620 |
| error response, 61 B | 194 | 146 | 45 | 527.741 |

stage 세 값의 합과 전체 API 시간의 차이는 envelope API와 반환 결과 처리 등 해당 세 stage 밖의 비용이다. 전체 시간은 micro 원값, 각 stage 시간은 구간 타이머로 따로 측정했으므로 합산해서 동일하게 나올 필요가 없다.

## Generator 및 wire 확인

NuGet Gallery에서 확인한 최신 안정판은 Grpc.Tools 2.85.0이다 ([NuGet Gallery](https://www.nuget.org/packages/Grpc.Tools/)). 내려받아 확인한 안정판과 포함된 protoc은 다음과 같다.

| Grpc.Tools | 포함 protoc | Editions C# generation |
|---|---:|---|
| 2.85.0 | 35.1 | 통과 |
| 2.84.0 | 35.1 | 통과 |
| 2.83.0 | 35.1 | 통과 |
| 2.82.0 | 35.0 | 통과 |
| 2.81.1 | 33.5 | 통과 |

최신 2.85.0 생성물로 Framework의 golden conformance 테스트 2개가 통과했다(유효 5개 encode/decode, 거절 8개 protocol error). 저장소 pin은 그대로 뒀다. Node는 protobufjs 8.8.1/protobufjs-cli 2.7.1, Java는 protoc 4.30.2(`libprotoc 30.2`)를 사용해 임시 Editions proto에서 생성했다. 두 언어 모두 13개 vector 처리가 통과했고 유효 5개의 encode bytes가 golden과 일치했다.

## 검증 및 정리

- C++ golden executable: assertion 활성화(`-UNDEBUG`) 상태로 유효 5개 및 reject 8개, 총 13개 통과.
- .NET: Grpc.Tools 2.85.0 source-binding Framework golden 테스트 2/2 통과.
- Node와 Java: 각 13/13 vector, 유효 encode 5/5 bytes 일치.
- 저장소의 임시 proto, C++ 계측 코드, CMake 타깃은 모두 복구했다. build 디렉터리를 제거했고 실험 원자료는 `/root/jobs/fx/cx/v21/s034/s2view/raw/`에 보존했다.
- 이 보고서 외에 저장소 소스 및 generator pin 변경은 없다.

DONE
