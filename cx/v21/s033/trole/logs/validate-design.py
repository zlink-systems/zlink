from pathlib import Path
import re, json, sys
root=Path('/root/worktree/zlink-v21-s033/trole')
job=Path('/root/jobs/fx/cx/v21/s033/trole')
execution=root/'framework/doc/framework/common/spec/server/01-execution'
specs={'S01':execution/'01-submit-and-completion.ko.md','S02':execution/'02-handler-turn-and-execution-gate.ko.md','S04':execution/'04-application-job-queue-and-backpressure.ko.md','S08':execution/'08-messaging-hot-path.ko.md'}
prefix={'C/':root/'framework/languages/cpp/framework/src/runtime','CT/':root/'framework/languages/cpp/tests/Zlink.Framework.UnitTests','J/':root/'framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime','N/':root/'framework/languages/node/packages/framework/src/runtime','D@6821/':None}
input_path=Path(sys.argv[1]) if len(sys.argv)>1 else job/'design.md'
s=input_path.read_text()
checks=[]
for m in re.finditer(r'(?P<prefix>D@6821/|CT/|C/|J/|N/)(?P<file>[\w./-]+):(?P<line>\d+)(?:[–-](?P<end>\d+))?',s):
 p=job/'logs/dotnet-6821c3b8af.cs' if m['prefix']=='D@6821/' else prefix[m['prefix']]/m['file']
 n=len(p.read_text().splitlines()) if p.exists() else 0
 end=int(m['end'] or m['line'])
 checks.append({'ref':m[0],'exists':p.exists(),'line_valid':0<int(m['line'])<=end<=n})
for m in re.finditer(r'(?P<spec>S01|S02|S04|S08):(?P<line>\d+)(?:[–-](?P<end>\d+))?',s):
 p=specs[m['spec']]; n=len(p.read_text().splitlines()); end=int(m['end'] or m['line'])
 checks.append({'ref':m[0],'exists':True,'line_valid':0<int(m['line'])<=end<=n})
quotes=[]
canonical=' '.join(specs['S08'].read_text().split()).replace('**','')
for line in s.splitlines():
 if not line.startswith('|') or 'S08:' not in line: continue
 for quote in re.findall('“([^”]+)”',line):
  needle=' '.join(quote.replace('**','').split())
  quotes.append({'quote':quote,'verbatim':needle in canonical})
result={'reference_checks':len(checks),'reference_failures':[c for c in checks if not(c['exists'] and c['line_valid'])],'quote_checks':len(quotes),'quote_failures':[q for q in quotes if not q['verbatim']]}
output_name='design-validation-negative.json' if len(sys.argv)>1 else 'design-validation.json'
(job/'logs'/output_name).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(result,ensure_ascii=False,indent=2))

sys.exit(1 if result['reference_failures'] or result['quote_failures'] else 0)
