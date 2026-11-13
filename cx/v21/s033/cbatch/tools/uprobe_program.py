from pathlib import Path

def build(pids,cond):
 libs=set()
 for pid in pids.values():
  for l in Path(f'/proc/{pid}/maps').read_text().splitlines():
   if '/libzlink.so' in l:libs.add(l.split()[-1])
 s=''
 for lib in libs:
  s+=f'''
uprobe:{lib}:_ZN5zlink9mailbox_t4sendERKNS_9command_tE /@on && ({cond})/ {{
 $command=*(uint32*)(arg1+8); @cmdsend[pid,tid,arg0,$command]=count();
 @cmdseen[pid,tid,$command]=@cmdseen[pid,tid,$command]+1;
 if (@cmdseen[pid,tid,$command]==4) {{ @cmdstack[pid,tid,$command,ustack(35)]=count(); }}
}}
uprobe:{lib}:_ZN5zlink9mailbox_t4recvEPNS_9command_tEib* /@on && ({cond})/ {{
 @recvcalls[pid,tid,arg0]=count(); @recvptr[tid]=arg1; @recvbox[tid]=arg0;
}}
uretprobe:{lib}:_ZN5zlink9mailbox_t4recvEPNS_9command_tEib* /@recvptr[tid]/ {{
 if (retval==0) {{ @cmdrecv[pid,tid,@recvbox[tid],*(uint32*)(@recvptr[tid]+8)]=count(); }}
 else {{ @recvempty[pid,tid,@recvbox[tid]]=count(); }}
 delete(@recvptr[tid]);delete(@recvbox[tid]);
}}
uprobe:{lib}:zlink_request,uprobe:{lib}:zlink_send_rid /@on && ({cond})/ {{
 @apiparts[pid,tid,probe,arg3]=count();
}}
uprobe:{lib}:zlink_reply /@on && ({cond})/ {{ @apiparts[pid,tid,probe,arg4]=count(); }}
uprobe:{lib}:zlink_send /@on && ({cond})/ {{ @apiparts[pid,tid,probe,arg2]=count(); }}
'''
 return s
