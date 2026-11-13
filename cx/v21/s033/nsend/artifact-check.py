import pathlib,json,hashlib
J=pathlib.Path(__file__).parent
rows=[];removed=[]
for commands in J.rglob('commands.json'):
 try:d=json.loads(commands.read_text())
 except (ValueError,OSError):continue
 cap=commands.parent/'capture'
 for role,pid in d.get('pids',{}).items():
  p=pathlib.Path(f'/tmp/perf-{pid}.map')
  if not p.exists() or pathlib.Path(f'/proc/{pid}').exists():continue
  data=p.read_bytes()
  if b'/root/worktree/zlink-nsend/' not in data:continue
  if (cap/f'{role}.perf.data').exists():
   dest=cap/f'{role}.perf.map';dest.write_bytes(data);rows.append({'source':str(p),'saved':str(dest),'sha256':hashlib.sha256(dest.read_bytes()).hexdigest()})
  p.unlink();removed.append(str(p))
(J/'temp-cleanup.json').write_text(json.dumps({'saved_maps':rows,'removed_own_temp_maps':removed},indent=2)+'\n')
print('saved native maps',len(rows),'removed own temp maps',len(removed))
