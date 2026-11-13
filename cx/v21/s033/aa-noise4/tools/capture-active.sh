#!/usr/bin/env bash
set -euo pipefail
dest="$1"
seconds="$2"
source_pid="${3:-}"
target_pid="${4:-}"
start_epoch="$(date +%s.%N)"
start_utc="$(date -u +%FT%TZ)"
int_before="$(cat /proc/interrupts)"
soft_before="$(cat /proc/softirqs)"
{
  printf 'active_start_utc=%s\n' "$start_utc"
  printf 'active_start_epoch=%s\n' "$start_epoch"
  printf 'cpu_frequency_samples_begin\n'
  printf 'cstate_disable_start_begin\n'
  for cpu in 0 8; do
    for state in 2 3; do
      state_dir="/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}"
      printf 'cpu%s.state%s.disable=%s\n' "$cpu" "$state" "$(cat "$state_dir/disable" 2>/dev/null || echo unavailable)"
    done
  done
  printf 'cstate_disable_start_end\n'
  printf 'cstate_usage_start_begin\n'
  for cpu in $(seq 0 15); do
    for state in 2 3; do
      state_dir="/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}"
      printf 'cpu%s.state%s.usage=%s\n' "$cpu" "$state" "$(cat "$state_dir/usage" 2>/dev/null || echo unavailable)"
    done
  done
  printf 'cstate_usage_start_end\n'
} >> "$dest"
mid_at=$((seconds / 2))
for ((sample=0; sample<seconds; sample++)); do
  sample_utc="$(date -u +%FT%TZ)"
  for cpu in $(seq 0 15); do
    freq="$(cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq" 2>/dev/null || echo unavailable)"
    printf '%s cpu%s.scaling_cur_freq_khz=%s\n' "$sample_utc" "$cpu" "$freq" >> "$dest"
  done
  if ((sample == mid_at)); then
    printf 'mid_active_utc=%s\n' "$sample_utc" >> "$dest"
    for pair in "source:$source_pid" "target:$target_pid"; do
      label="${pair%%:*}"; pid="${pair#*:}"
      [[ -n "$pid" ]] || continue
      { printf '\n%s_mid_sched_pid=%s\n' "$label" "$pid"
        for task in /proc/"$pid"/task/*; do
          [[ -d "$task" ]] || continue
          tid="${task##*/}"
          printf '%s_tid=%s\n' "${label}_mid" "$tid"
          policy="$(chrt -p "$tid" 2>&1 || true)"
          printf '%s\n' "$policy" >> "$dest"
          if ! grep -q 'SCHED_OTHER' <<<"$policy"; then printf 'scheduling_invalid=%s_mid:%s\n' "$label" "$tid" >> "$dest"; fi
        done
      } >> "$dest"
    done
    cat /proc/loadavg | sed 's/^/mid_active_loadavg=/' >> "$dest"
    ps -eo pid,cls,%cpu,stat,args --sort=-%cpu | head -45 >> "$dest"
  fi
  sleep 1
done
end_utc="$(date -u +%FT%TZ)"
end_epoch="$(date +%s.%N)"
int_after="$(cat /proc/interrupts)"
soft_after="$(cat /proc/softirqs)"
{
  printf 'active_end_utc=%s\n' "$end_utc"
  printf 'active_end_epoch=%s\n' "$end_epoch"
  printf 'loadavg_end='; cat /proc/loadavg
  printf 'cstate_disable_end_begin\n'
  for cpu in 0 8; do
    for state in 2 3; do
      state_dir="/sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}"
      printf 'cpu%s.state%s.disable=%s\n' "$cpu" "$state" "$(cat "$state_dir/disable" 2>/dev/null || echo unavailable)"
    done
  done
  printf 'cstate_disable_end_end\n'
  printf 'cstate_usage_delta_begin\n'
  python3 - "$dest" <<'PY'
import re,sys
path=sys.argv[1]
text=open(path,encoding='utf-8',errors='replace').read()
before={}
for cpu,state,value in re.findall(r'^cpu(\d+)\.state(\d+)\.usage=(\d+)$',text,re.M):
    before[(int(cpu),int(state))]=int(value)
for cpu in range(16):
    for state in (2,3):
        p=f'/sys/devices/system/cpu/cpu{cpu}/cpuidle/state{state}/usage'
        try: after=int(open(p).read().strip())
        except (OSError,ValueError): after=None
        start=before.get((cpu,state))
        delta='unavailable' if after is None or start is None else str(after-start)
        print(f'cpu{cpu}.state{state}.usage_delta={delta}')
PY
  printf 'cstate_usage_delta_end\n'
  printf 'interrupts_delta_begin\n'
  python3 - "$int_before" "$int_after" <<'PY'
import re,sys
def parse(text):
    out={}
    for line in text.splitlines():
        m=re.match(r'\s*(\S+):\s+((?:\d+\s+)+)\s*(.*)',line)
        if m: out[m.group(3).strip()]=(list(map(int,m.group(2).split())),m.group(1))
    return out
a,b=map(parse,sys.argv[1:])
for name in sorted(a.keys()|b.keys()):
    av,ai=a.get(name,([],'')); bv,bi=b.get(name,([],''))
    n=max(len(av),len(bv)); delta=[(bv[i] if i<len(bv) else 0)-(av[i] if i<len(av) else 0) for i in range(n)]
    print(f'{name}: vectors={",".join(map(str,delta))}')
PY
  printf 'softirqs_delta_begin\n'
  python3 - "$soft_before" "$soft_after" <<'PY'
import re,sys
def parse(text):
    out={}
    for line in text.splitlines():
        m=re.match(r'\s*(\S+):\s+((?:\d+\s*)+)$',line)
        if m: out[m.group(1)]=list(map(int,m.group(2).split()))
    return out
a,b=map(parse,sys.argv[1:])
for name in sorted(a.keys()|b.keys()):
    av,bv=a.get(name,[]),b.get(name,[]); n=max(len(av),len(bv))
    print(f'{name}: vectors={",".join(str((bv[i] if i<len(bv) else 0)-(av[i] if i<len(av) else 0)) for i in range(n))}')
PY
  printf 'guard_log_lines_begin\n'
  python3 - "$start_utc" "$end_utc" /root/jobs/fx/cx/quiet/guard.log <<'PY'
import datetime,sys
def dt(s): return datetime.datetime.fromisoformat(s.replace('Z','+00:00'))
s,e=map(dt,sys.argv[1:3]); p=sys.argv[3]
try:
    for line in open(p,encoding='utf-8',errors='replace'):
        token=line[:20]
        try: t=dt(token)
        except ValueError: continue
        if s<=t<=e: print(line,end='')
except FileNotFoundError: print('guard.log unavailable')
PY
} >> "$dest"
