#!/usr/bin/env python3
import pathlib,re,sys
src=pathlib.Path(sys.argv[1]); dst=pathlib.Path(sys.argv[2]); pending={}; count=0
pat=re.compile(r' (\d+)/(\d+) \[\d+\] ([0-9.]+):\s+raw_syscalls:sys_(enter|exit): NR (\d+)(?: \(([^)]*)\)| = (-?\d+))')
with src.open(errors='replace') as f,dst.open('w') as o:
 for line in f:
  m=pat.search(line)
  if not m: continue
  pid,tid,ts,phase,nr,args,ret=m.groups(); nr=int(nr)
  if phase=='enter': pending[tid]=(pid,ts,nr,args or '')
  else:
   ent=pending.pop(tid,None)
   if ent and ent[2]==nr and nr in (44,45,46,47):
    fd=ent[3].split(',')[0].strip() if ent[3] else ''
    length=ent[3].split(',')[2].strip() if nr in (44,46) and len(ent[3].split(','))>2 else ''
    o.write(f'{ent[0]}\t{tid}\t{nr}\t{fd}\t{ent[1]}\t{ts}\t{ret}\t{length}\t{line.strip()}\n'); count+=1
print(count, dst.stat().st_size)
