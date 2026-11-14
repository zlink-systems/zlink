
## 00:59 UTC — guard가 멈춘 프로세스를 깨우지 마십시오 (감독자)
- 다른 job이 measure.lock을 exclusive로 잡으면 guard가 빌드·시험·git을 SIGSTOP으로 멈춥니다(상태 T). 이것은 정상입니다.
- `kill -CONT`로 깨우지 마십시오(00:52 UTC에 `kill -CONT -- -2570602`를 실행했습니다). 측정 중인 다른 언어의 숫자를 망칩니다. lock이 풀리면 guard가 자동으로 다시 실행합니다. 기다리는 동안 코드를 읽거나 보고서를 쓰십시오.
