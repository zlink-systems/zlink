#!/usr/bin/env python3
"""OS, public runtime and immutable package provenance. No machine tuning."""
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CORE_HASH = json.loads((Path(__file__).resolve().parent/'launchers.json').read_text())['requiredCoreSha256']


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''): h.update(chunk)
    return h.hexdigest()


def read(path):
    try: return Path(path).read_text().strip()
    except FileNotFoundError: return None


def cgroup_read(name):
    # In cgroup v2 the root lacks controller limits; the current process scope owns them.
    group=next((line.split(':',2)[2] for line in Path('/proc/self/cgroup').read_text().splitlines() if line.startswith('0::')), '/')
    current=Path('/sys/fs/cgroup')/group.lstrip('/')/name
    value=read(current)
    return value if value is not None else read(Path('/sys/fs/cgroup')/name)


def command_output(command):
    try:
        p=subprocess.run(command,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
        return {'command':command,'exitCode':p.returncode,'output':p.stdout}
    except (OSError,subprocess.TimeoutExpired) as error:
        return {'command':command,'errorType':type(error).__name__,'message':str(error)}


def native_file(prefix):
    p=Path(prefix)
    candidates=[p/'lib/libzlink.so',p/'lib64/libzlink.so',p/'libzlink.so']
    candidates.extend(p.glob('**/libzlink.so'))
    for candidate in candidates:
        if candidate.is_file() and digest(candidate)==CORE_HASH: return candidate.resolve()
    raise ValueError('ArtifactMismatch: prefix has no fixed Core 0.18.0 library with required digest: '+str(p))


def version(path):
    values=dict(line.split('=',1) for line in Path(path).read_text().splitlines() if '=' in line)
    return next(value for key,value in values.items() if key.endswith('_VERSION'))


def collect(manifest=None):
    affinity=sorted(os.sched_getaffinity(0));limits=resource.getrlimit(resource.RLIMIT_NOFILE)
    cpu=next((line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),platform.processor())
    prefixes=[os.environ.get('ZLINK_CORE_PACKAGE_PREFIX'),os.environ.get('ZLINK_CPP_CORE_PACKAGE_PREFIX')]
    core=None
    if manifest and manifest.get('corePackagePrefix'): prefixes.insert(0,manifest['corePackagePrefix'])
    for prefix in prefixes:
        if prefix:
            core=native_file(prefix);break
    result={'schemaVersion':3,'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)), 'buildMode':'Release',
            'frameworkVersions':{lang:version(ROOT/'framework/languages'/lang/'VERSION') for lang in ('cpp','dotnet','java','node')},'bindingVersions':{lang:version(ROOT/'bindings'/lang/'VERSION') for lang in ('cpp','dotnet','java','node')},'coreVersion':version(ROOT/'VERSION'),'requiredCoreSha256':CORE_HASH,
            'cpuModel':cpu,'effectiveProcessorCount':len(affinity),'cpuAffinity':affinity,'cpuQuota':cgroup_read('cpu.max'),
            'cpuset':cgroup_read('cpuset.cpus.effective'),'memoryLimit':cgroup_read('memory.max'),
            'memoryCurrent':cgroup_read('memory.current'),'loadAverage':list(os.getloadavg()),
            'os':platform.platform(),'kernel':platform.release(),'host':platform.node(),'container':Path('/.dockerenv').exists(),
            'fdLimit':{'soft':str(limits[0]),'hard':str(limits[1])},'ephemeralPortRange':read('/proc/sys/net/ipv4/ip_local_port_range'),
            'listenBacklog':read('/proc/sys/net/core/somaxconn'),'tcpMaxSynBacklog':read('/proc/sys/net/ipv4/tcp_max_syn_backlog'),
            'tcpTimeWaitReuse':read('/proc/sys/net/ipv4/tcp_tw_reuse'),'cgroup':read('/proc/self/cgroup'),
            'runtimeVersions':{name:command_output(command) for name,command in {'dotnet':['dotnet','--info'],'java':['java','-version'],
                'node':['node','--version'],'cpp':['c++','--version']}.items()},
            'runtimeOptions':{key:os.environ.get(key) for key in ('DOTNET_PROCESSOR_COUNT','DOTNET_GCHeapHardLimit','DOTNET_gcServer',
                'DOTNET_ThreadPool_ForceMinWorkerThreads','DOTNET_ThreadPool_ForceMaxWorkerThreads','JAVA_TOOL_OPTIONS','JDK_JAVA_OPTIONS','NODE_OPTIONS')},
            'deployment':'same-host loopback; participants share CPU resources','serializer':{'name':'default public Framework typed JSON',
                'options':'Base64 logical payload; no custom message codec or compression'},'artifacts':[]}
    if manifest:
        java_home=manifest['languages']['java'].get('environment',{}).get('JAVA_HOME')
        if java_home:result['runtimeVersions']['java']=command_output([str(Path(java_home)/'bin/java'),'-version'])
        result['participantRuntimeEnvironments']={lang:entry.get('environment',{}) for lang,entry in manifest['languages'].items()}
    if core:
        provenance=Path(prefix)/'share/zlink/core-package-provenance.json'
        result['corePackage']={'actualLibraryPath':str(core),'sha256':digest(core),'prefix':str(Path(prefix).resolve()),
            'packageProvenanceFile':str(provenance),'packageProvenance':json.loads(provenance.read_text()) if provenance.is_file() else None}
        if not provenance.is_file():raise ValueError('ArtifactMismatch: public Core package provenance missing')
    for name in ('histogram-bounds-ns.json','matrix.json','schema.json','metric-catalog.json'):
        p=Path(__file__).resolve().parent/name
        result['artifacts'].append({'path':str(p),'sha256':digest(p)})
    return result

if __name__=='__main__':
    data=json.dumps(collect(),indent=2)+'\n'
    if len(sys.argv)==1: print(data,end='')
    elif len(sys.argv)==2:
        with open(sys.argv[1],'x') as stream:stream.write(data)
    else:raise SystemExit('usage: environment.py [new-output-file]')
