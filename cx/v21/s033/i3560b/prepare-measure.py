from pathlib import Path
old=Path('/root/jobs/fx/cx/v21/s033/i3560/measure-condB.py').read_text()
old=old.replace("ROOT = pathlib.Path('/root/worktree/zlink-v21-s033/i3560')", "ROOT = pathlib.Path('/root/worktree/zlink-v21-s033/i3560b')")
old=old.replace("OUT = pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560')", "OUT = pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b')")
old=old.replace("STAMP = 'i3560-core124-condB'", "MODE=os.environ.get('PROBE_MODE','stacks')\nSTAMP = 'i3560b-'+MODE")
a=old.index('def bpf_script('); b=old.index('\nrun_dir = ',a)
old=old[:a]+'''def bpf_script(source, target):
    pids=f'(pid == {source} || pid == {target})'
    s=['BEGIN { @gate=0; @sec=0; }']
    def probe(kind, sym, body, gate=True):
        s.append(f'{kind}:{LIB}:{sym} /{pids}'+(' && @gate' if gate else '')+f'/ {{ {body} }}')
    probe('uprobe','zlink_request','@requests[pid,tid,arg0,arg4]=count();')
    probe('uretprobe','zlink_request','if ((int32)retval == 0) { @request_ok[pid,tid]=count(); }')
    if MODE == 'stacks':
        probe('uprobe','_ZN5zlink9mailbox_t4sendERKNS_9command_tE','@mail[tid]=*(uint32*)(arg1+8)+1; @mailbox[tid]=arg0; @mailcommand[pid,tid,arg0,*(uint32*)(arg1+8)]=count();')
        probe('uretprobe','_ZN5zlink9mailbox_t4sendERKNS_9command_tE','delete(@mail[tid]); delete(@mailbox[tid]);',False)
        probe('uprobe','_ZN5zlink13socket_base_t25notify_request_completionEv','@notify[tid]=arg0; @notify_socket[pid,tid,arg0]=count();')
        probe('uretprobe','_ZN5zlink13socket_base_t25notify_request_completionEv','delete(@notify[tid]);',False)
        probe('uprobe','_ZN5zlink10signaler_t4sendEv','@send[pid,tid,@mail[tid],@notify[tid],usym(*(uint64*)reg("sp")),arg0]=count(); @stacks[pid,tid,ustack(perf,16)]=count(); @sendptr[tid]=arg0;')
        probe('uretprobe','_ZN5zlink10signaler_t4sendEv','delete(@sendptr[tid]);',False)
        probe('uprobe','_ZN5zlink10signaler_t13recv_failableEv','@recv[pid,tid,usym(*(uint64*)reg("sp")),arg0]=count(); @recvptr[tid]=arg0;')
        probe('uretprobe','_ZN5zlink10signaler_t13recv_failableEv','delete(@recvptr[tid]);',False)
        s.append(f'tracepoint:syscalls:sys_enter_write /{pids} && @gate && @sendptr[tid]/ {{ @writes[pid,tid,@sendptr[tid]]=count(); }}')
        s.append(f'tracepoint:syscalls:sys_enter_read /{pids} && @gate && @recvptr[tid]/ {{ @reads[pid,tid,@recvptr[tid]]=count(); }}')
    elif MODE == 'api':
        probe('uprobe','zlink_completion_recv','@comp_handle[tid]=arg0; @comp_flags[tid]=arg2; @comp_enter[pid,tid,arg0,arg2]=count();')
        probe('uretprobe','zlink_completion_recv','@comp_result[pid,tid,@comp_handle[tid],@comp_flags[tid],(int32)retval]=count(); delete(@comp_handle[tid]); delete(@comp_flags[tid]);')
        probe('uprobe','zlink_poller_wait','@poller[tid]=arg0; @wait_enter[pid,tid,arg0,(int64)arg3]=count();')
        probe('uretprobe','zlink_poller_wait','@wait_result[pid,tid,@poller[tid],(int32)retval]=count(); delete(@poller[tid]);')
        probe('uprobe','zlink_router_recv','@router_handle[tid]=arg0; @router_enter[pid,tid,arg0,arg5]=count();')
        probe('uretprobe','zlink_router_recv','@router_result[pid,tid,@router_handle[tid],(int32)retval]=count(); delete(@router_handle[tid]);')
        probe('uprobe','zlink_reply','@reply[pid,tid,arg0]=count();')
        probe('uprobe','_ZN5zlink8object_t15process_commandERKNS_9command_tE','@command[pid,tid,*(uint32*)(arg1+8)]=count();')
    s.append('interval:s:1 { @sec=@sec+1; if (@sec==1) { @gate=1; } if (@sec==3) { @gate=0; } if (@sec==4) { exit(); } }')
    return '\\n'.join(s)+'\\n'
''' +old[b:]
old=old.replace("OUT / 'raw-condB'", "OUT / ('raw-'+MODE)")
old=old.replace("RUNS='2'", "RUNS=os.environ.get('MEASURE_RUNS','1')")
old=old.replace("RUN_STAMP=STAMP, OUTPUT_DIR=str(run_dir)", "RUN_STAMP=STAMP, OUTPUT_DIR=str(run_dir), SOURCE_CPUS='0-7', TARGET_CPUS='8-15'")
# Attach registration probe before process creation; it records only this candidate library.
old=old.replace("runner = subprocess.Popen", '''registration=None
if MODE=='api':
    reg=run_dir/'registration.bt'
    reg.write_text(f'uprobe:{LIB}:zlink_socket {{ @type[tid]=arg1; }}\\nuretprobe:{LIB}:zlink_socket {{ printf("socket pid=%d tid=%d type=%d socket=%lx\\\\n",pid,tid,@type[tid],retval); delete(@type[tid]); }}\\nuretprobe:{LIB}:zlink_poller_new {{ printf("poller pid=%d tid=%d poller=%lx\\\\n",pid,tid,retval); }}\\nuprobe:{LIB}:zlink_poller_add {{ printf("add pid=%d tid=%d poller=%lx socket=%lx mask=%d\\\\n",pid,tid,arg0,arg1,arg3); }}\\nuprobe:{LIB}:zlink_connect {{ printf("connect pid=%d tid=%d socket=%lx endpoint=%s\\\\n",pid,tid,arg0,str(arg1)); }}\\nuprobe:{LIB}:zlink_bind {{ printf("bind pid=%d tid=%d socket=%lx endpoint=%s\\\\n",pid,tid,arg0,str(arg1)); }}\\n')
    reglog=open(run_dir/'registration.txt','w')
    registration=subprocess.Popen(['bpftrace','-q',str(reg)],stdout=reglog,stderr=subprocess.STDOUT)
    time.sleep(2)
runner = subprocess.Popen''')
old=old.replace("                script=cell/'wake.bt'; script.write_text(bpf_script(src,tgt))", """                if MODE=='cpu':
                    perf_log=open(cell/'perf-record.log','w')
                    perf=subprocess.Popen(['perf','record','-F','199','--call-graph','dwarf,8192','-p',f'{src},{tgt}','-o',str(cell/'perf.data'),'--','sleep','4'],stdout=perf_log,stderr=subprocess.STDOUT)
                script=cell/'wake.bt'; script.write_text(bpf_script(src,tgt))""")
old=old.replace("                time.sleep(0.15)", """                if MODE=='cpu':
                    perf.wait(timeout=8); perf_log.close()
                time.sleep(0.15)""")
old=old.replace("    log.close()", """    log.close()
    if registration is not None:
        registration.send_signal(2); registration.wait(timeout=5); reglog.close()""")
old=old.replace("OUT/'capture-summary.json'", "OUT/('capture-'+MODE+'-summary.json')")
Path('/root/jobs/fx/cx/v21/s033/i3560b/measure.py').write_text(old)
