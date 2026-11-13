#!/usr/bin/env python3
import pathlib,re,subprocess
ref='494c5f4827db8d86a5dce716b7a8da6e7e9cf970'
base='framework/languages/cpp/framework/src/runtime/mesh/'
for name in ['raw_mesh_node_owner.cpp','raw_mesh_node_owner.hpp','service_topology_registry.cpp','service_topology_registry.hpp']:
    p=pathlib.Path(base+name)
    current=p.read_text()
    normal=re.sub(r'^#if defined\s*\(ZLINK_CPP_ABLATION_LADDER\).*?^#endif\n','',current,flags=re.M|re.S)
    expected=subprocess.check_output(['git','show',ref+':'+str(p)],text=True)
    lines=lambda s:[line for line in s.splitlines() if line.strip()]
    assert lines(normal)==lines(expected),str(p)
    print(f'{p}: normal runtime matches {ref} (experiment blocks and blank lines removed)')
