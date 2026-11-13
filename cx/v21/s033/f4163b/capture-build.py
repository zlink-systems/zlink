import pathlib,sys,json,hashlib,subprocess,shutil
j=pathlib.Path(__file__).parent
kind=sys.argv[1]; src=j/('src-'+kind); label=sys.argv[2] if len(sys.argv)>2 else kind
out=j/'logs'/label;out.mkdir(exist_ok=True)
for rel in ['core/build/core-release-gate/compile_commands.json','core/build/core-release-gate/CMakeCache.txt','bindings/c/build-release-1.23.0/compile_commands.json','bindings/c/build-release-1.23.0/CMakeCache.txt']:
 p=src/rel;shutil.copy2(p,out/(('core-' if rel.startswith('core/') else 'harness-')+p.name))
prefix=j/('prefix-'+label);runtime=prefix/'lib/libzlink.so.1.23.0'
for name,cmd in [('sections.txt',['readelf','-SW',str(runtime)]),('symbols.txt',['nm','-anC',str(runtime)]),('ldd.txt',['ldd',str(src/'bindings/c/build-release-1.23.0/perf/perf_router_router')])]:
 (out/name).write_bytes(subprocess.check_output(cmd))
shutil.copy2(src/'core/build/core-release-gate/CMakeFiles/libzlink.dir/link.txt',out/'core-link.txt')
commands=json.loads((out/'core-compile_commands.json').read_text())
assert commands and all('-O3' in x['command'] and '-g ' not in x['command'] for x in commands)
assert '-flto' in (out/'core-CMakeCache.txt').read_text() or '-flto' in (src/'core/build/core-release-gate/CMakeFiles/libzlink.dir/link.txt').read_text()
assert '.debug_info' not in (out/'sections.txt').read_text()
if kind=='L': assert all('-falign-functions=64' in x['command'] for x in commands if x['file'].endswith('.cpp'))
(out/'source.diff').write_bytes(subprocess.check_output(['git','-C',str(src),'diff']))
print(label,hashlib.sha256(runtime.read_bytes()).hexdigest(),'O3/LTO/no-g verified')
