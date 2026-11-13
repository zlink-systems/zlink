#!/usr/bin/env python3
from pathlib import Path
import os, re

me=os.getpid(); ancestors={me}
pid=os.getppid()
while pid > 1 and pid not in ancestors:
    ancestors.add(pid)
    try: pid=int(Path(f'/proc/{pid}/stat').read_text().split()[3])
    except (OSError, ValueError, IndexError): break
pattern=re.compile(r'(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)')
found=[]
for entry in Path('/proc').iterdir():
    if not entry.name.isdigit() or int(entry.name) in ancestors: continue
    try:
        status=(entry/'stat').read_text().split()
        state=status[2]
        if state not in ('R','S'): continue
        comm=(entry/'comm').read_text().strip()
        args=(entry/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        if pattern.search(comm+' '+args): found.append((int(entry.name),state,comm,args[:180]))
    except (OSError, IndexError): continue
for item in found: print(*item)
raise SystemExit(bool(found))
