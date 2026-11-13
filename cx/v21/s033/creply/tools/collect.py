import argparse,concurrent.futures,ctypes,json,os,pathlib,signal,subprocess,time,threading
p=argparse.ArgumentParser()
for n in ('mode','source','target','source-url','target-url','out'):p.add_argument('--'+n,required=True)
a=p.parse_args();out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
import urllib.request
def stats(url):
 with urllib.request.urlopen(url+'/bench/stats',timeout=8) as r:return json.load(r)
def counter():
 t=time.monotonic();s={'source':stats(a.source_url),'target':stats(a.target_url)}
 return {'start':t,'end':time.monotonic(),'stats':s}
def snapshot():
 d={'monotonic':time.monotonic(),'realtime':time.time(),'stats':counter()['stats'],'processes':{}}
 for role,pid in pids.items():
  threads={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'stat':(t/'stat').read_text(),'status':(t/'status').read_text()}
   except FileNotFoundError:pass
  fds={}
  for f in pathlib.Path(f'/proc/{pid}/fd').iterdir():
   try:fds[f.name]={'link':os.readlink(f),'info':pathlib.Path(f'/proc/{pid}/fdinfo/{f.name}').read_text()}
   except FileNotFoundError:pass
  d['processes'][role]={'pid':pid,'threads':threads,'fds':fds,'maps':pathlib.Path(f'/proc/{pid}/maps').read_text()}
 d['tcp']=pathlib.Path('/proc/net/tcp').read_text();d['tcp6']=pathlib.Path('/proc/net/tcp6').read_text()
 d['end_monotonic']=time.monotonic();return d
initial=snapshot();(out/'initial.json').write_text(json.dumps(initial,indent=2))
port=5247 if ':5246' in a.source_url else 5254
io=[int(t) for r in pids for t,v in initial['processes'][r]['threads'].items() if 'IO/' in v['comm']]
cond=' || '.join(f'tid == {t}' for t in io);wakecond=' || '.join(f'args->pid == {t}' for t in io)
cp=os.getpid();libc=ctypes.CDLL(None,use_errno=True)
children=[];handles=[];ssrows=[];stop=threading.Event()
def ss_sample():
 while not stop.is_set():
  begin=time.monotonic();s=subprocess.check_output(['ss','-tinmeo','( sport = :'+str(port)+' or dport = :'+str(port)+' )'],text=True)
  ssrows.append({'start':begin,'end':time.monotonic(),'ss':s,'stats':counter(),'cpu_stat':pathlib.Path('/proc/stat').read_text(),'frequency_khz':{str(i):pathlib.Path(f'/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq').read_text().strip() for i in range(16)}})
  stop.wait(2)
def start(cmd,name):
 (out/(name+'.command.json')).write_text(json.dumps(cmd));h=open(out/(name+'.log'),'w');handles.append(h)
 c=subprocess.Popen(cmd,stdout=h,stderr=h);children.append(c);return c

def finish(c):
 if c.poll() is None:c.send_signal(signal.SIGINT)
 try:c.wait(timeout=20)
 except subprocess.TimeoutExpired:c.terminate();c.wait(timeout=20)
 if c.returncode not in (0,130,-2):raise RuntimeError((c.args,c.returncode))

def bpf(mode,seconds):
 d=out/mode;d.mkdir(exist_ok=True)
 program=f'''BEGIN {{ printf("READY\\n"); }}
tracepoint:syscalls:sys_enter_getpgid /pid == {cp}/ {{
 if (args->pid == {pids['source']}) {{ @on=1; printf("START %llu\\n",nsecs); }}
 if (args->pid == {pids['target']}) {{ @on=0; printf("STOP %llu\\n",nsecs); }}
}}
'''
 if mode=='syscall':
  program+=f'''
tracepoint:raw_syscalls:sys_enter /@on && (pid == {pids['source']} || pid == {pids['target']}) && (args->id == 44 || args->id == 45)/ {{
 @id[tid]=args->id; @fd[tid]=args->args[0]; @size[tid]=args->args[2]; @begin[tid]=nsecs;
 @callcpu[pid,tid,args->id,cpu]=count();
}}
tracepoint:raw_syscalls:sys_exit /@id[tid]/ {{
 $id=@id[tid];$fd=@fd[tid];$size=@size[tid];$ret=args->ret;
 @calls[pid,tid,$fd,$id]=count(); @requested[pid,tid,$fd,$id]=sum($size);
 @requested_hist[pid,tid,$fd,$id,$size < 8192 ? $size / 256 : 32]=count();
 @status[pid,tid,$fd,$id,$ret < 0 ? $ret : 0]=count();
 @elapsed_ns[pid,tid,$fd,$id]=sum(nsecs-@begin[tid]);
 if ($ret > 0) {{
  @positive[pid,tid,$fd,$id]=count();@bytes[pid,tid,$fd,$id]=sum($ret);
  @returned_hist[pid,tid,$fd,$id,$ret < 8192 ? $ret / 256 : 32]=count();
  if ($ret < $size) {{ @short[pid,tid,$fd,$id]=count(); }}
 }}
 delete(@id[tid]);delete(@fd[tid]);delete(@size[tid]);delete(@begin[tid]);
}}
END {{ clear(@on); clear(@id);clear(@fd);clear(@size);clear(@begin); }}
'''
 elif mode=='net':
  program+=f'''
tracepoint:net:net_dev_start_xmit /@on/ {{
 $skb=(struct sk_buff*)args->skbaddr;
 $ip=(uint8*)($skb->head+$skb->network_header);
 if ((*$ip >> 4) == 4 && *($ip+9) == 6) {{
  $tcp=(uint8*)($skb->head+$skb->transport_header);$sport=bswap(*(uint16*)$tcp);$dport=bswap(*(uint16*)($tcp+2));
  if ($sport == {port} || $dport == {port}) {{
   $len=bswap(*(uint16*)($ip+2)) - ((*$ip & 15)*4) - ((*($tcp+12) >> 4)*4);
   @skb[$sport,$dport,$len > 0 ? 1 : 0]=count();@skb_bytes[$sport,$dport]=sum($len);
   @skb_hist[$sport,$dport,$len < 8192 ? $len/256 : 32]=count();
   @gso[$sport,$dport,args->gso_segs,args->gso_size]=count();
   @txcpu[$sport,$dport,pid,tid,cpu]=count();
  }}
 }}
}}
'''
  for funid,fun in enumerate(('tcp_send_ack','tcp_send_delayed_ack','tcp_delack_timer_handler','tcp_retransmit_skb','tcp_ack','tcp_add_backlog','tcp_rcv_established','tcp_data_queue','tcp_queue_rcv','sock_def_readable')):
   program+=f'''
kfunc:{fun} /@on/ {{
 $sk=args->sk;$sport=$sk->__sk_common.skc_num;$dport=bswap($sk->__sk_common.skc_dport);
 if ($sport == {port} || $dport == {port}) {{
  @events[{funid},$sport,$dport]=count();
  @event_context[{funid},$sport,$dport,pid,tid,cpu]=count();
'''
   if fun=='tcp_ack':
    program+='  @socket_options[$sport,$dport,((struct tcp_sock*)$sk)->nonagle,$sk->sk_sndbuf,$sk->sk_rcvbuf,$sk->sk_userlocks]=count();\n'
   if fun=='tcp_send_ack':
    program+='  @ackmode[$sport,$dport,((struct inet_connection_sock*)$sk)->icsk_ack.quick > 0 ? 1 : 0]=count();\n'
   program+=' }\n}\n'
  program+=f'kfunc:tcp_do_parse_auth_options /@on/ {{ $sp=bswap(args->th->source);$dp=bswap(args->th->dest);if ($sp == {port} || $dp == {port}) {{ @auth_parse[$sp,$dp,pid,tid]=count(); }} }}\n'
  program+='END {clear(@on);}\n'
 elif mode=='mutex':
  binary=os.readlink(f"/proc/{pids['target']}/exe")
  symbol='_ZN5zlink9framework6detail7backend16raw_route_port_t5replyERKNS2_14raw_received_tESt6vectorINS_9message_tESaIS8_EE'
  libcpath='/lib/x86_64-linux-gnu/libc.so.6'
  program+=f"""
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
"""
 elif mode=='handoff':
  lib='/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so'
  program+=f'''
tracepoint:sched:sched_waking /@on && ({wakecond})/ {{
 @waking[args->pid,pid,tid,cpu,args->target_cpu]=count();
}}
tracepoint:sched:sched_wakeup /@on && ({wakecond})/ {{
 @wakeup[args->pid,pid,tid,cpu,args->target_cpu]=count();
}}
tracepoint:sched:sched_switch /@on/ {{
'''
  prev=' || '.join(f'args->prev_pid == {t}' for t in io)
  program+=' if (@last[cpu]) { @runtime[args->prev_pid,cpu,args->prev_comm]=sum(nsecs-@last[cpu]); } @last[cpu]=nsecs;\n'
  program+=f' if ({prev}) {{ @switchout[args->prev_pid,args->prev_state]=count(); }}\n}}\n'
  program+=f'''
uprobe:{lib}:zlink_reply /@on && pid == {pids['target']}/ {{
 @reply[tid,cpu,comm]=count();
 if (!@seen_reply[tid]) {{ @reply_stack[tid,ustack(24)]=count();@seen_reply[tid]=1; }}
}}
END {{clear(@on);clear(@last);clear(@seen_reply);}}
'''
 elif mode=='completion':
  lib='/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so'
  for funid,(label,symbol) in enumerate([('sync_write','_ZN5zlink15tcp_transport_t10write_someEPKhm'),('async_write','_ZN5zlink15tcp_transport_t16async_write_someEPKhmSt8functionIFvRKN5boost6system10error_codeEmEE'),('write_complete','_ZN5zlink13asio_engine_t17on_write_completeERKN5boost6system10error_codeEm')]):
   program+=f'uprobe:{lib}:{symbol} /@on && ({cond})/ {{ @completion[{funid},pid,tid]=count(); }}\n'
  program+='END {clear(@on);}'
 else:raise ValueError(mode)
 (d/'program.bt').write_text(program)
 c=start(['bpftrace','-q','-f','json','-B','line',str(d/'program.bt')],mode)
 deadline=time.monotonic()+25
 while 'READY' not in (out/(mode+'.log')).read_text():
  if c.poll() is not None:raise RuntimeError('BPF compile failed: '+str(out/(mode+'.log')))
  if time.monotonic()>deadline:raise RuntimeError('BPF readiness timeout')
  time.sleep(.05)
 before=snapshot();b=counter();libc.getpgid(pids['source']);time.sleep(seconds);libc.getpgid(pids['target']);e=counter();after=snapshot();finish(c)
 (d/'before.json').write_text(json.dumps(before,indent=2));(d/'after.json').write_text(json.dumps(after,indent=2));(d/'window.json').write_text(json.dumps({'before':b,'after':e},indent=2))
 print(mode,'rate',(e['stats']['source']['completed']-b['stats']['source']['completed'])/((e['start']+e['end']-b['start']-b['end'])/2),flush=True)

def perf(seconds):
 perfprocs=[]
 for r in pids:
  tids=','.join(t for t,v in initial['processes'][r]['threads'].items() if 'IO/' in v['comm'])
  perfprocs.append(start(['perf','record','-e','cycles:k','-F','249','-g','-t',tids,'-o',str(out/(r+'.perf.data'))],r+'-perf'))
 time.sleep(.4);b=counter();before=snapshot()
 for r in pids:
  tids=','.join(t for t,v in initial['processes'][r]['threads'].items() if 'IO/' in v['comm'])
  start(['perf','stat','-e','cycles:k,instructions:k,task-clock,context-switches,cpu-migrations','-t',tids,'--per-thread','-x',',','--timeout',str(int(seconds*1000)),'-o',str(out/(r+'.cycles.csv'))],r+'-stat')
 time.sleep(seconds+.1);after=snapshot();e=counter()
 for c in perfprocs:finish(c)
 (out/'perf-before.json').write_text(json.dumps(before,indent=2));(out/'perf-after.json').write_text(json.dumps(after,indent=2));(out/'perf-window.json').write_text(json.dumps({'before':b,'after':e},indent=2))
 print('perf rate',(e['stats']['source']['completed']-b['stats']['source']['completed'])/((e['start']+e['end']-b['start']-b['end'])/2),flush=True)

signal.signal(signal.SIGTERM,lambda *x: (_ for _ in ()).throw(KeyboardInterrupt()))
t=threading.Thread(target=ss_sample);t.start()
try:
 if a.mode=='perf':perf(25)
 elif a.mode=='details':bpf('syscall',6);bpf('net',6);bpf('handoff',5)
 elif a.mode=='mutex':bpf('mutex',6)
 elif a.mode=='syscall':bpf('syscall',10)
 elif a.mode=='net':bpf('net',10)
 elif a.mode=='handoff':bpf('handoff',5);bpf('completion',5)
 elif a.mode=='combined':
  perf(20);bpf('syscall',6);bpf('net',6)
 else:raise ValueError(a.mode)
finally:
 stop.set();t.join();(out/'ss.json').write_text(json.dumps(ssrows,indent=2))
 for c in children:
  if c.poll() is None:c.send_signal(signal.SIGINT)
 for c in children:
  try:c.wait(timeout=20)
  except subprocess.TimeoutExpired:c.terminate();c.wait(timeout=20)
 for h in handles:h.close()
 (out/'exit-codes.json').write_text(json.dumps([c.returncode for c in children]))
