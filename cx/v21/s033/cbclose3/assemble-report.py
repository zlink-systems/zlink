from pathlib import Path
import json
r=Path(__file__).parent
d=json.loads((r/'benchmark-results.json').read_text())
complete=all(d['stats'].get(p,{}).get('pairs')==6 for p in ['PAIR','PUBSUB','REQUEST_SERIAL'])
bench='''cbclose2/bench-pair.py와 bench-all.py를 복사하여 출력 경로와 PR serial 바이너리 경로만 바꾸었다. Main tree는 cbclose2/main-tree, main library는 cbclose2/build-main이며 main을 재빌드하거나 수정하지 않았다. PAIR·PUBSUB은 기존 suite binary, 직렬 request는 cbclose2/serial.cpp의 public API 프로그램이다. Payload 1024 byte, inproc, PERF_PART_COUNT=1, PERF_SINGLE_DURATION_SECONDS=5, taskset -c 8-15다. PAIR·PUBSUB 단위는 msg/s, REQUEST_SERIAL은 request/s다.

홀수 쌍은 main→PR, 짝수 쌍은 PR→main이다. 각 쌍은 flock -o /root/jobs/fx/cx/measure.lock 안에서 두 process를 연속 실행하며 쌍 내부에는 다른 측정이 들어가지 않는다. 각 쌍 실행 전 uptime과 1분 load를 기록하고 load가 4를 넘으면 실행 없이 연기한다. 각 process 직전에 MemAvailable 12 GiB 이상을 검사한다. 이번 build/test는 모두 끝낸 뒤 benchmark를 시작하였다. 다른 작업의 shared/exclusive lock을 기다린 시간은 측정 시간에 포함하지 않는다.

binary-provenance.json과 ldd-main.log·ldd-after.log는 main/PR library·binary와 Core package를 기록한다. Main library, PAIR, PUBSUB, serial hash는 기존 cbclose2/binary-provenance.json과 모두 일치한다. Core SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. CMakeCache-after.txt와 cbclose2/CMakeCache-main.txt에서 compiler·Core·Release·LTO OFF 조건이 같다.

'''
bench+=(r/'benchmark-summary.txt').read_text()+'\n'
if complete:
 s=d['stats']['PAIR'];met=s['median']>=.98
 bench+=f"PAIR 중앙값 {s['median']:.6f}은 목표 0.98을 {'충족하였다' if met else '충족하지 못했다'}. "
 if met:bench+='따라서 조건부 perf stat/record 단계는 실행하지 않았다.\n\n'
 else:bench+='Perf 진단 결과는 아래 기록을 따른다.\n\n'
 loads=[x['load1'] for x in d['pairs']]
 mem=[x['memory_kB'] for p in d['pairs'] for x in p['runs']]
 bench+=f"유효한 18쌍의 실행 전 1분 load는 {min(loads):.6f}–{max(loads):.6f}, process 직전 최소 MemAvailable은 {min(mem)/1024**2:.3f} GiB다. 모든 실행의 exit는 0이다. 개별 수치를 제외하거나 쌍을 다시 골라 측정하지 않았다.\n\n"
else:bench+='아직 측정 중이다. 위 숫자는 완료된 쌍만 포함하며 최종 합격 판정이 아니다.\n\n'
(r/'report.md').write_text((r/'report-base.txt').read_text()+bench+(r/'report-end.txt').read_text())
print('report complete measurements:',complete)
