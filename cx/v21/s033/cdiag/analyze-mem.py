from pathlib import Path
import re,collections,json
j=Path(__file__).parent
roots=[('send-saturation','main',j/'mem-repeat/send-saturation/main'),('send-saturation','branch',j/'mem/send-saturation/branch'),('request-backpressure','main',j/'mem/request-backpressure/main'),('request-backpressure','branch',j/'mem/request-backpressure/branch')]
rx=re.compile(r'(\d+)/(\d+)\s+(\d+\.\d+): ibs_op//:\s+([0-9a-f]+)\s+([0-9a-f]+) \|OP ([^|]+)\|LVL ([^|]+)\|SNP ([^|]+)\|TLB ([^|]+)\|LCK ([^|]+)\|BLK\s+\S+\s+([0-9a-f]+)\s+(.*) \((.*)\)')
results=[]
for pattern,variant,root in roots:
 ds=list(root.glob('run1/*/capture'))
 if not ds:continue
 d=ds[0];p=d/'mem.script'
 if not p.exists() or p.stat().st_size==0:continue
 target_pid=str(json.loads((d/'before.json').read_text())['processes']['target']['pid'])
 roles=set(re.findall(r'ROLE app-worker tid=(\d+)',(d.parent/'target.log').read_text()))
 counts=collections.Counter();levels=collections.Counter();snoops=collections.Counter();counter=collections.Counter();shared=collections.defaultdict(set);sharedsites=collections.defaultdict(collections.Counter)
 for line in p.open():
  m=rx.search(line)
  if not m or m[1]!=target_pid or m[2] not in roles:continue
  counts['all_app_op_samples']+=1
  if m[13]=='[kernel.kallsyms]':counts['app_kernel_op_samples']+=1;continue
  if m[6] not in ('LOAD','STORE'):continue
  counts['app_user_memory_op_samples']+=1
  levels[m[7].strip()]+=1;snoops[m[8].strip()]+=1
  if m[10].strip() not in ('N/A','No'):counts['app_user_locked_memory_op_samples']+=1
  symbol=m[12]
  if any(s in symbol for s in ('_Sp_counted_base','application_work_started','application_work_finished','application_work_enqueued','application_job_queue_t::release_credits','application_job_queue_t::permit_t::mark_queued','service_mailbox_t::settle_once','service_mailbox_t::settles_last')):
   counts['visible_counter_memory_op_samples']+=1
   counter[symbol]+=1
   address=int(m[4],16)
   if address:
    key=hex(address&~63);shared[key].add(m[2]);sharedsites[key][symbol]+=1
 lines=[{'address':k,'worker_tids':sorted(tids),'sites':sharedsites[k].most_common(4),'samples':sum(sharedsites[k].values())} for k,tids in shared.items() if len(tids)>1]
 lines.sort(key=lambda x:-x['samples'])
 counts['counter_lines_seen_on_multiple_workers']=len(lines)
 row={'pattern':pattern,'variant':variant,'directory':str(d),'counts':counts,'levels':levels,'snoops':snoops,'counter_sites':counter.most_common(20),'shared_counter_lines':lines[:30]}
 results.append(row)
 print(pattern,variant,dict(counts),'levels',dict(levels),'snoops',dict(snoops))
(j/'memory-analysis.json').write_text(json.dumps(results,indent=2))
