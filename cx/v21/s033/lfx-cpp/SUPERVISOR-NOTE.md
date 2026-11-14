
## 00:52 UTC — 측정 우선 (PM, 11:00 KST까지)
- 빌드가 끝나기를 기다리지 마십시오(600초 build wait 없음). 측정할 준비가 되면 바로 `flock -x -w 5400 /root/jobs/fx/cx/measure.lock <runner>`로 **blocking** 대기합니다. lock을 잡으면 guard가 빌드·git을 SIGSTOP으로 멈춥니다.
- 자기 빌드는 lock 밖에서 끝낸 뒤 측정합니다. 감독자가 pre-pr 시험을 멈춰 두었습니다.
- timing: warmup 1초(runner 최소), active 5초, run 사이 3초, 버리는 warm run 없음.
