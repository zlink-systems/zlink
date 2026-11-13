from pathlib import Path
p=Path('/root/jobs/fx/cx/v21/s033/creply/tools/collect.py');s=p.read_text()
s=s.replace(" elif mode=='handoff':",''' elif mode=='mutex':
  binary=os.readlink(f"/proc/{pids['target']}/exe")
  symbol='_ZN5zlink9framework6detail7backend16raw_route_port_t5replyERKNS2_14raw_received_tESt6vectorINS_9message_tESaIS8_EE'
  libcpath='/lib/x86_64-linux-gnu/libc.so.6'
  program+=f\"\"\"
uprobe:{binary}:{symbol} /@on && pid == {pids['target']}/ {{ @inreply[tid]=1;@first[tid]=1;@reply_calls=count(); }}
uretprobe:{binary}:{symbol} /pid == {pids['target']}/ {{delete(@inreply[tid]);delete(@first[tid]);}}
uprobe:{libcpath}:pthread_mutex_lock /@on && pid == {pids['target']} && @inreply[tid] && @first[tid]/ {{
 @wait[tid]=nsecs;@mutex[tid]=arg0;delete(@first[tid]);
}}
uretprobe:{libcpath}:pthread_mutex_lock /pid == {pids['target']} && @wait[tid]/ {{
 $ns=nsecs-@wait[tid];@lock_calls[@mutex[tid]]=count();@lock_ns[@mutex[tid]]=sum($ns);@lock_hist_ns=hist($ns);
 @lock_by_tid_ns[tid]=sum($ns);delete(@wait[tid]);delete(@mutex[tid]);
}}
tracepoint:sched:sched_switch /@on/ {{
 if (@wait[args->prev_pid]) {{@off[args->prev_pid]=nsecs;@switch_wait[args->prev_pid,args->prev_state]=count();}}
 if (@off[args->next_pid]) {{@lock_offcpu_ns[args->next_pid]=sum(nsecs-@off[args->next_pid]);delete(@off[args->next_pid]);}}
}}
tracepoint:syscalls:sys_enter_futex /@on && pid == {pids['target']} && @wait[tid]/ {{@lock_futex[tid,args->op & 127]=count();}}
END {{ clear(@on);clear(@wait);clear(@mutex);clear(@inreply);clear(@first);clear(@off);}}
\"\"\"
 elif mode=='handoff':''')
s=s.replace("elif a.mode=='details':bpf('syscall',6);bpf('net',6);bpf('handoff',5)","elif a.mode=='details':bpf('syscall',6);bpf('net',6);bpf('handoff',5)\n elif a.mode=='mutex':bpf('mutex',6)")
p.write_text(s)
