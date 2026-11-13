from pathlib import Path
import re
j=Path('/root/jobs/fx/cx/v21/s033/f4163')
lines=['series,run,records,qmean,qmax,cached_minus_actual,waits,refusals,read_wake,flushes,mean_ms,rate']
for kind in ['probe-C','probe-M']:
 for p in sorted((j/'runs').glob(kind+'-*.log')):
  s=p.read_text(); probe=[l for l in s.splitlines() if l.startswith('PROBE4163')]
  if not probe: continue
  d=dict(re.findall(r'(\w+)=(\d+)',probe[-1])); results={r[5]:r[6] for l in s.splitlines() if l.startswith('RESULT,') and len(r:=l.split(','))==7}
  lines.append(','.join(map(str,[kind,p.stem.split('-')[-1],d['records'],round(int(d['qsum'])/int(d['samples']),2),d['qmax'],round((int(d['cached_sum'])-int(d['qsum']))/int(d['samples']),3),d['waits'],d['refusals'],d['wake'],d['flush'],results.get('latency',''),results.get('throughput','')])))
(j/'probe-summary.csv').write_text('\n'.join(lines)+'\n'); print('\n'.join(lines))
