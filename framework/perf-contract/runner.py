#!/usr/bin/env python3
"""One coordinator for four public Framework applications and all default cells."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
from environment import ROOT, collect, digest, native_file, CORE_HASH
from results import aggregate, write_json
from validator import validate_result
HERE=Path(__file__).resolve().parent
MATRIX=json.loads((HERE/'matrix.json').read_text())
DEFAULTS=MATRIX['defaults']
HTTP=urllib.request.build_opener(urllib.request.ProxyHandler({}))

class InvalidSetupError(RuntimeError): pass
class UnsupportedCellError(RuntimeError): pass


def positive_number(value):
    number=float(value)
    if not 0 < number < float('inf'): raise argparse.ArgumentTypeError('finite positive number required')
    return number


def positive_int(value):
    number=int(value)
    if number <= 0: raise argparse.ArgumentTypeError('positive integer required')
    return number


def options(argv):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation',choices=('matrix','single','diagnostic'))
    p.add_argument('--language',choices=MATRIX['languages'])
    p.add_argument('--languages',default=','.join(MATRIX['languages']))
    p.add_argument('--scenario',choices=sorted({c['scenario'] for c in MATRIX['cells']}))
    p.add_argument('--mode',choices=sorted({c['mode'] for c in MATRIX['cells']}))
    p.add_argument('--terminal',choices=('ordinary','yield'))
    p.add_argument('--spot-count',type=positive_int)
    p.add_argument('--channel-topology',choices=('routemesh','clientserver'))
    for key in ('connections','logical-streams','client-count','connect-concurrency','inflight','subscriber-count'):
        p.add_argument('--'+key,type=positive_int)
    p.add_argument('--warmup-seconds',type=positive_number)
    p.add_argument('--duration-seconds',type=positive_number)
    p.add_argument('--runs',type=positive_int,default=1)
    p.add_argument('--codec',choices=('json',),default='json')
    p.add_argument('--worker-task-millis',type=positive_int)
    p.add_argument('--worker-pool-size',type=positive_int)
    p.add_argument('--comparison-config',type=Path)
    p.add_argument('--workload-config',type=Path)
    p.add_argument('--executables-manifest',type=Path,default=HERE/'launchers.json')
    p.add_argument('--output',type=Path)
    p.add_argument('--run-id',default=time.strftime('%Y%m%dT%H%M%SZ',time.gmtime())+'-'+uuid.uuid4().hex[:10])
    p.add_argument('--skip-build',action='store_true')
    p.add_argument('--prepare-only',action='store_true',help='Setup probe and preserve originals; no warmup/measured phase')
    args=p.parse_args(argv)
    if args.runs!=1:p.error("Every selected cell uses runs=1; repeated runs are not authorized")
    comparable=('scenario','mode','terminal','spot_count','channel_topology','connections','logical_streams','client_count','connect_concurrency','inflight','subscriber_count','warmup_seconds','duration_seconds','worker_task_millis','worker_pool_size')
    if args.comparison_config and any(getattr(args,key) is not None for key in comparable): p.error('comparison-config owns comparable fields; duplicate CLI consumers are forbidden')
    if args.workload_config and any(getattr(args,key) is not None for key in comparable): p.error('workload-config owns workload fields; duplicate CLI consumers are forbidden')
    if args.comparison_config and args.workload_config:
        common=json.loads(args.comparison_config.read_text())
        if 'workload' in common or 'optionalManifest' in common:p.error('comparison-config and workload-config contain duplicate workload owners')
    args.selected_languages=[args.language] if args.language else args.languages.split(',')
    if len(set(args.selected_languages))!=len(args.selected_languages) or any(l not in MATRIX['languages'] for l in args.selected_languages):p.error('languages must be a distinct list of known languages')
    if args.operation!='matrix' and not args.scenario and not args.comparison_config and not args.workload_config:p.error('single/diagnostic requires scenario or manifest')
    args.output=(args.output or ROOT/'framework/perf-results'/args.run_id).resolve()
    if args.output.exists():p.error('output already exists; originals must never be overwritten')
    return args


def http_json(url,body=None,timeout=5):
    request=urllib.request.Request(url,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
    with HTTP.open(request,timeout=timeout) as response:return json.load(response)


def get_json(url,timeout=5):return http_json(url,timeout=timeout)


def parallel(items,callback):
    if not items:return []
    with ThreadPoolExecutor(max_workers=len(items)) as pool:return list(pool.map(callback,items))


class OwnedProcesses:
    def __init__(self,cell,env):
        self.cell=cell;self.env=env;self.processes=[];self.logs=[];self.reservations=[];self.controls=[]
    def reserve(self):
        s=socket.socket(socket.AF_INET,socket.SOCK_STREAM);s.bind(('127.0.0.1',0));self.reservations.append(s);return s.getsockname()[1]
    def release(self,ports):
        for s in list(self.reservations):
            if s.getsockname()[1] in ports:s.close();self.reservations.remove(s)
    def start(self,name,command,ports=(),client=False):
        self.release(ports);log=(self.cell/'logs'/(name+'.log')).open('xb');self.logs.append(log)
        process=subprocess.Popen(command,cwd=ROOT,env=self.env,stdin=subprocess.PIPE if client else subprocess.DEVNULL,
            stdout=subprocess.PIPE if client else log,stderr=log,close_fds=True)
        self.processes.append((name,process));write_json(self.cell/'tmp'/(name+'-process.json'),{'pid':process.pid,'command':command})
        return process
    def check(self):
        for name,p in self.processes:
            if p.poll() is not None:raise RuntimeError(f'{name} PID {p.pid} exited {p.returncode}; see logs/{name}.log')
    def cleanup(self):
        for s in self.reservations:s.close()
        self.reservations=[]
        evidence=[]
        # Request graceful stop to all owned children before any wait.
        controlled={c.process.pid:c for c in self.controls}
        for name,p in self.processes:
            if p.poll() is None:
                try:
                    if p.pid in controlled:controlled[p.pid].send('stop')
                    else:p.send_signal(signal.SIGINT)
                except (BrokenPipeError,ProcessLookupError,OSError) as e:evidence.append({'process':name,'stage':'graceful','error':str(e)})
        for stage,bound in (('graceful',5),('terminate',5),('kill',2)):
            if stage!='graceful':
                for name,p in self.processes:
                    if p.poll() is None:
                        try:p.terminate() if stage=='terminate' else p.kill()
                        except ProcessLookupError:pass
                        evidence.append({'process':name,'stage':stage,'pid':p.pid})
            deadline=time.monotonic()+bound
            while any(p.poll() is None for _,p in self.processes) and time.monotonic()<deadline:time.sleep(.02)
        for name,p in self.processes:
            if p.poll() is None:raise RuntimeError('Owned PID did not reap within shutdown bounds: '+str(p.pid))
            p.wait()
        write_json(self.cell/'cleanup.json',{'boundsSeconds':{'graceful':5,'terminate':5,'killAndReap':2},'escalations':evidence,
            'ownedProcesses':[{'name':name,'pid':p.pid,'exitCode':p.returncode,'reaped':True} for name,p in self.processes]})
        for c in self.controls:c.log.close()
        for log in self.logs:log.close()


class ClientControl:
    def __init__(self, process: subprocess.Popen, log: Path, prepared_path: Path | None = None):
        self.process = process
        self.log = log.open("ab")
        self.buffer = b""
        self.prepared_path = prepared_path

    def receive(self, seconds: float, prepared: bool = False) -> dict:
        deadline = time.monotonic() + seconds
        while True:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                self.log.write(line + b"\n")
                self.log.flush()
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue  # Diagnostic text is preserved; only typed control JSON is evidence.
                if not isinstance(value, dict) or "ok" not in value:
                    continue
                if value.get("type") == "prepared":
                    if self.prepared_path is not None and not self.prepared_path.exists():
                        write_json(self.prepared_path, value)
                    if prepared and not value["ok"]:
                        raise InvalidSetupError("Client preparation failed; original envelope and snapshot preserved")
                    return value
                if prepared:
                    raise InvalidSetupError("Client did not return the declared prepared envelope")
                if not value["ok"]:
                    raise RuntimeError("Client control failure: " + json.dumps(value))
                return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Client JSON control acknowledgement exceeded its configured bound")
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            if not readable:
                raise TimeoutError("Client JSON control acknowledgement timed out")
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("Client control pipe closed before acknowledgement")
            self.buffer += data

    def send(self, command: str, request: dict | None = None) -> None:
        value = {"command": command}
        if request is not None:
            value["request"] = request
        self.process.stdin.write((json.dumps(value) + "\n").encode())
        self.process.stdin.flush()

    def call(self, command: str, request: dict | None = None, seconds: float = 5) -> object:
        self.send(command, request)
        deadline = time.monotonic() + seconds
        while True:
            envelope = self.receive(max(0, deadline - time.monotonic()))
            if envelope.get("type") != "prepared":
                value = envelope["response"]
                break
        if isinstance(value, dict) and (value.get("accepted") is False or value.get("ok") is False):
            raise RuntimeError("Phase acknowledgement rejected: " + json.dumps(value))
        return value

    def close(self) -> None:
        if self.process.poll() is None:
            self.send("stop")
            # Shared owner performs one simultaneous bounded shutdown.
            pass
        self.log.close()



class RunRedis:
    def __init__(self,output,settings):self.output=output;self.settings=settings;self.cli=None;self.container=None;self.metadata=None
    def invoke(self,*args):
        p=subprocess.run([self.cli,*args],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
        if p.returncode:raise InvalidSetupError('Docker '+args[0]+': '+p.stderr.strip())
        return p.stdout.strip()
    def ensure(self):
        if self.metadata:return self.metadata
        errors=[]
        for cli in ('docker','docker.exe'):
            try:
                p=subprocess.run([cli,'version','--format','{{json .Server}}'],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
                if p.returncode==0:
                    self.cli=cli;engine=json.loads(p.stdout);break
                errors.append({'command':cli,'message':p.stderr.strip()})
            except (OSError,subprocess.TimeoutExpired) as e:errors.append({'command':cli,'message':str(e)})
        if not self.cli:raise InvalidSetupError('Run-owned Redis Docker engine unavailable; no host Redis fallback: '+json.dumps(errors))
        try:self.invoke('image','inspect',self.settings['image'])
        except InvalidSetupError:
            # Image acquisition precedes the bounded container startup phase.
            pulled=subprocess.run([self.cli,'pull',self.settings['image']],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120)
            if pulled.returncode:raise InvalidSetupError('Docker Redis image acquisition failed: '+pulled.stderr)
        self.container=self.invoke('run','-d','--rm','--cpus',str(self.settings['cpus']),'--memory',self.settings['memory'],
            '-p','127.0.0.1::6379',self.settings['image'],'redis-server','--save','','--appendonly','no')
        inspect=json.loads(self.invoke('inspect',self.container))[0]
        published=inspect['NetworkSettings']['Ports']['6379/tcp']
        if len(published)!=1:raise InvalidSetupError('Redis must expose one run-owned loopback port')
        port=int(published[0]['HostPort'])
        deadline=time.monotonic()+10
        last=None
        while time.monotonic()<deadline:
            try:
                # Infrastructure probe only; measured Framework messages stay on public APIs.
                with socket.create_connection(('127.0.0.1',port),timeout=1) as s:
                    s.sendall(b'*1\r\n$4\r\nPING\r\n')
                    if s.recv(64)==b'+PONG\r\n':break
            except OSError as e:last=e
            time.sleep(.05)
        else:raise InvalidSetupError('Run-owned Docker Redis endpoint did not respond within startup10s: '+str(last))
        image=json.loads(self.invoke('image','inspect',inspect['Image']))[0]
        self.metadata={'containerId':self.container,'endpoint':'127.0.0.1:'+str(port),'publishedHostIp':published[0]['HostIp'],
            'imageRequested':self.settings['image'],'imageId':inspect['Image'],'imageDigests':image.get('RepoDigests',[]),
            'resources':{'nanoCpus':inspect['HostConfig']['NanoCpus'],'memoryBytes':str(inspect['HostConfig']['Memory'])},
            'dockerExecutable':self.cli,'engine':engine,'redisVersion':self.invoke('exec',self.container,'redis-server','--version'),
            'lifetime':'one container owned by this run; fresh namespace per cell'}
        write_json(self.output/'redis.json',self.metadata)
        return self.metadata
    def close(self):
        if self.container:
            identifier=self.container
            try:
                self.invoke('stop','-t','5',identifier)
                remaining=self.invoke('ps','-a','--filter','id='+identifier,'--format','{{.ID}}')
                if remaining:raise InvalidSetupError('Run-owned Redis container remains after graceful stop: '+identifier)
                write_json(self.output/'redis-cleanup.json',{'containerId':identifier,'stopRequested':True,'removed':True,'verification':'docker ps -a --filter id=<exact owned ID>'})
            except (OSError,ValueError,InvalidSetupError,subprocess.TimeoutExpired) as error:
                write_json(self.output/'redis-cleanup.json',{'containerId':identifier,'stopRequested':True,'removed':False,'errorType':type(error).__name__,'message':str(error)})
                raise
            self.container=None


def workload(args,cell):
    result={key:DEFAULTS[key] for key in ('requestPayloadBytes','responsePayloadBytes','sendPayloadBytes','durationSeconds','warmupSeconds','inflight','clientCount',
        'connectConcurrency','requestTimeoutMs','correlationExpiryMs','settleTimeoutMs','setupTimeoutMs','adminTimeoutMs','socketSendTimeoutMs',
        'applicationDeadlineMs','workerTaskMillis','workerPoolSize','subscriberCount')}
    result.update(connections=cell['connections'],logicalStreams=cell['logicalStreams'])
    for key in ('connections','logicalStreams','clientCount','connectConcurrency','inflight','subscriberCount','warmupSeconds','durationSeconds','workerTaskMillis','workerPoolSize'):
        attr=''.join('_'+c.lower() if c.isupper() else c for c in key)
        override=getattr(args,attr,None)
        if override is not None:result[key]=override
    if args.comparison_config:
        common=json.loads(args.comparison_config.read_bytes())
        supplied=common.get('workload',{})
        unknown=set(supplied)-set(result)
        if unknown:raise UnsupportedCellError('No application workload consumer: '+','.join(sorted(unknown)))
        result.update(supplied)
    if 'optionalManifest' in cell:
        supplied=cell['optionalManifest']
        aliases={'measuredSeconds':'durationSeconds'}
        result.update({aliases.get(key,key):value for key,value in supplied.items() if aliases.get(key,key) in result})
    if (result['requestPayloadBytes'],result['responsePayloadBytes'],result['sendPayloadBytes'])!=(64,4096,4096):raise InvalidSetupError('Logical payload sizes are fixed at 64/4096/4096')
    if result['applicationDeadlineMs']>result['settleTimeoutMs']:raise InvalidSetupError('Business deadline must fit settle bound')
    cs=cell['streamTransport'] is not None
    if not cs:result['connections']=None;result['connectConcurrency']=None;result['clientCount']=1
    else:result['logicalStreams']=None
    return result


def selected_cells(args):
    if args.comparison_config and not args.workload_config:
        data=json.loads(args.comparison_config.read_bytes())
        cells=data.get('cells',[data.get('cell',data)])
        selected=[]
        for item in cells:
            matches=[dict(c) for c in MATRIX['cells'] if all(c.get(key)==item.get(key) for key in ('scenario','mode','terminal','spotCount','topology') if key in item)]
            if len(matches)!=1:raise InvalidSetupError('comparison-config must select an exact public matrix cell')
            selected.append(matches[0])
        return selected
    if args.workload_config:
        manifest=json.loads(args.workload_config.read_bytes())
        if 'scenario' not in manifest:raise InvalidSetupError('workload-config requires scenario')
        cells=[dict(c) for c in MATRIX['cells'] if c['scenario']==manifest['scenario']]
        if not cells:raise UnsupportedCellError('Manifest scenario has no public application consumer')
        for key in ('mode','terminal','spotCount','topology'):
            if key in manifest:cells=[c for c in cells if c[key]==manifest[key]]
        if len(cells)!=1:raise InvalidSetupError('workload-config must select one exact scenario/mode/terminal/Spot/topology cell')
        cells[0]['optionalManifest']=manifest
        return cells
    cells=[dict(c) for c in MATRIX['cells']]
    for attr,key in (('scenario','scenario'),('mode','mode'),('terminal','terminal'),('spot_count','spotCount'),('channel_topology','topology')):
        value=getattr(args,attr)
        if value is not None:cells=[c for c in cells if c[key]==value]
    if not cells:raise InvalidSetupError('No exact common matrix cell matches selectors')
    if args.operation!='matrix' and len(cells)>1:
        defaults=[c for c in cells if c['terminal']=='ordinary' and c['spotCount']==16 and c['topology']!='clientserver']
        if len(defaults)==1:cells=defaults
        else:raise InvalidSetupError('single/diagnostic requires unambiguous terminal/Spot/topology selection')
    return cells


def comparison_input(args,cell,env,redis_settings):
    if args.comparison_config:
        exact=args.comparison_config.read_bytes(); supplied=json.loads(exact)
        actual={'affinity':env.get('cpuAffinity'),'cpuQuota':env.get('cpuQuota'),'cpuset':env.get('cpuset'),'memoryLimit':env.get('memoryLimit'),
            'effectiveProcessorCount':env.get('effectiveProcessorCount'),'hostSharing':'same-host loopback; shared CPUs','queueProfile':'Framework default',
            'requiredCoreSha256':CORE_HASH,'redis':redis_settings}
        for key,value in supplied.get('resourcePlan',{}).items():
            if key not in actual or value != actual[key]:raise InvalidSetupError('Comparison resourcePlan has no matching actual consumer: '+key)
        for key,value in {'serializer':'default Framework typed JSON; Base64 logical payload; no custom codec',
            'diagnostics':'Normal' if args.operation=='diagnostic' else 'Off'}.items():
            if key in supplied and supplied[key]!=value:raise InvalidSetupError('Comparison configuration does not match actual '+key)
        return exact
    data={'schemaVersion':3,'cell':{key:cell[key] for key in cell if key!='optionalManifest'},'workload':workload(args,cell),
          'resourcePlan':{'affinity':env['cpuAffinity'],'cpuQuota':env['cpuQuota'],'cpuset':env['cpuset'],'memoryLimit':env['memoryLimit'],
              'effectiveProcessorCount':env['effectiveProcessorCount'],'hostSharing':'same-host loopback; shared CPUs',
              'queueProfile':'Framework default','requiredCoreSha256':CORE_HASH,'redis':redis_settings},
          'serializer':'default Framework typed JSON; Base64 logical payload; no custom codec',
          'diagnostics':'Normal' if args.operation=='diagnostic' else 'Off'}
    if 'optionalManifest' in cell:data['optionalManifest']=cell['optionalManifest']
    return (json.dumps(data,sort_keys=True,separators=(',',':'),ensure_ascii=False)+'\n').encode()


def launch_command(manifest,language,kind):
    commands=manifest['languages'][language]
    command=list(commands.get(kind,commands['server']))
    for index,arg in enumerate(command):
        if (ROOT/arg).exists():command[index]=str((ROOT/arg).resolve())
    return command


def role_specs(cell):
    scenario=cell['scenario']
    if scenario=='session-echo-only':return [('session',0,False,'None','session')]
    if scenario=='cs-local-session-actor-echo':return [('session',0,False,'Server','session')]
    if scenario=='cs-remote-session-actor-echo':return [('actor',0,False,'Server','server'),('session',0,False,'Client','session')]
    if cell['mode']=='publish':return [('subscriber',i,False,'None','server') for i in range(cell['workload']['subscriberCount'])]+[('publisher',0,True,'None','server')]
    if scenario.startswith('actor-'):return [('actor',0,False,'Server','server'),('actorCaller',0,True,'Client','server')]
    if scenario.startswith('s2s-channel-to-spot'):return [('spot',0,False,'Server','server'),('channel',0,True,'Client','server')]
    if scenario.startswith('s2s-spot-to-channel'):return [('channel',0,False,'None','server'),('spot',0,True,'Server','server')]
    if scenario.startswith('spot-'):return [('spot',0,True,'Server','server')]
    return [('channel',1,False,'None','server'),('channel',0,True,'None','server')]


def build_languages(args,manifest):
    for language in args.selected_languages:
        if args.skip_build:continue
        log=args.output/('build-'+language+'.log')
        if language=='dotnet':
            commands=[['dotnet','build',str(ROOT/'framework/languages/dotnet/perf'/('ZLink.Framework.Perf.'+role)/('ZLink.Framework.Perf.'+role+'.csproj')),
                '-c','Release','-m:1','--nologo'] for role in ('ChannelServer','SessionServer','Client')]
        elif language=='java':commands=[['./gradlew','--no-daemon','--max-workers=1','-p','perf','installDist']]
        elif language=='node':commands=[['npm','--prefix','framework/languages/node','run','build']]
        else:
            cpp=manifest['languages']['cpp']; configured=cpp.get('build',{})
            build_environment=dict(os.environ);build_environment.update(cpp.get('environment',{}))
            prefix=configured.get('localPackageRoot') or build_environment.get('ZLINK_LOCAL_PACKAGE_ROOT')
            if not prefix:raise InvalidSetupError('C++ build requires public package ZLINK_LOCAL_PACKAGE_ROOT; source binding builds are forbidden')
            commands=[['cmake','-S','framework/languages/cpp/perf','-B','framework/languages/cpp/perf/build','-DCMAKE_BUILD_TYPE=Release',
                '-DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=OFF','-DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT='+prefix,
                '-DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST='+('ON' if configured.get('useSystemBoost',True) else 'OFF')],
                ['cmake','--build','framework/languages/cpp/perf/build','--target','zlink_framework_perf','--parallel','1']]
        if language=='cpp':
            dependency_prefix=configured.get('cmakePrefixPath') or build_environment.get('CMAKE_PREFIX_PATH')
            toolchain=configured.get('toolchainFile') or build_environment.get('CMAKE_TOOLCHAIN_FILE')
            if dependency_prefix:commands[0].append('-DCMAKE_PREFIX_PATH='+dependency_prefix)
            if toolchain:commands[0].append('-DCMAKE_TOOLCHAIN_FILE='+toolchain)
            for argument in configured.get('cmakeArguments',[]):
                if not isinstance(argument,str) or not argument.startswith('-D') or 'USE_BINDINGS_SOURCE' in argument:raise InvalidSetupError('C++ cmakeArguments must be public -D options and cannot enable source bindings')
                commands[0].append(os.path.expandvars(argument))
        write_json(args.output/('build-'+language+'-provenance.json'),{'commands':commands,'environment':manifest['languages'][language].get('environment',{}),
            'buildConfiguration':manifest['languages'][language].get('build',{}),'bindingsSource':False,
            'cppDependencyPrefix':dependency_prefix if language=='cpp' else None,'cppToolchain':toolchain if language=='cpp' else None})
        with log.open('xb') as stream:
            for command in commands:
                cwd=ROOT/'framework/languages/java' if language=='java' else ROOT
                build_env=dict(os.environ);build_env.update(manifest['languages'][language].get('environment',{}))
                p=subprocess.run(command,cwd=cwd,env=build_env,stdout=stream,stderr=subprocess.STDOUT)
                if p.returncode:raise InvalidSetupError('Build failed: '+str(log)+' (first failure preserved)')


def wait_ready(owned,roles,full,cell,stage,seconds):
    deadline=time.monotonic()+seconds;observed={};pending=list(roles)
    try:
        while pending:
            owned.check()
            for role in list(pending):
                key=role['role']+'-'+str(role['roleInstance'])
                try:
                    value=get_json(role['metrics']['baseUrl']+'/perf/ready',min(1,max(.001,deadline-time.monotonic())))
                    observed[key]=value
                    if value['ready' if full else 'infrastructureReady']:pending.remove(role)
                    elif any('failed' in item.lower() for item in value.get('reasons',[])):raise InvalidSetupError('Public readiness failure: '+json.dumps(value))
                except (urllib.error.URLError,ConnectionError,TimeoutError) as error:observed[key]={'errorType':type(error).__name__,'message':str(error)}
            if time.monotonic()>=deadline:raise TimeoutError(stage+' readiness exceeded '+str(seconds)+'s')
            if pending:time.sleep(.02)
    finally:write_json(cell/'tmp'/(stage+'-readiness.json'),observed)
    return observed


def make_roles(args,cell_config,owned,redis):
    plan=role_specs(cell_config);role_configs=[];roles=[];ports=[]
    scenario=cell_config['scenario'];cs=cell_config['streamTransport'] is not None
    # All transport/admin ports are held by OS reservations until their owner starts.
    for role,instance,source,objects,kind in plan:
        stream=owned.reserve() if role=='session' else None
        mesh=owned.reserve() if cell_config['topology']!='clientserver' and cell_config['mode']!='publish' and scenario!='session-echo-only' else None
        channel=owned.reserve() if cell_config['topology']=='clientserver' and not source else None
        fanout=owned.reserve() if cell_config['mode']=='publish' and source else None
        admin,trigger=owned.reserve(),owned.reserve()
        ports.append({'stream':stream,'mesh':mesh,'channel':channel,'fanout':fanout,'admin':admin,'trigger':trigger})
    spot_ids=['perf-spot-'+str(i) for i in range(cell_config['spotCount'])]
    actor_count=cell_config['workload']['connections'] if cs else cell_config['workload']['logicalStreams']
    actor_ids=['perf-actor-'+str(i) for i in range(actor_count or 0)] if 'actor' in scenario else []
    store_needed=cell_config['mode']=='publish' or any(objects!='None' for _,_,_,objects,_ in plan)
    store=None
    if store_needed:
        info=redis.ensure();store={'provider':'redis','endpoint':info['endpoint'],'namespace':'perf:'+args.run_id+':'+uuid.uuid4().hex+':'}
    for index,(role,instance,source,objects,kind) in enumerate(plan):
        p=ports[index]
        tcp=lambda port:None if port is None else 'tcp://127.0.0.1:'+str(port)
        peers=[] if store_needed else [tcp(q['mesh']) for j,q in enumerate(ports) if j!=index and q['mesh'] is not None]
        listener='ws://127.0.0.1:'+str(p['stream']) if p['stream'] else tcp(p['mesh'] or p['channel'] or p['fanout'])
        peer=next((tcp(q['channel']) for q in ports if q['channel']),None) if cell_config['topology']=='clientserver' and source else None
        if scenario=='channel-echo-only' and source and cell_config['topology']=='routemesh':peer=next(iter(peers),None)
        role_config={'runId':args.run_id,'cellId':cell_config['cellId'],'configHash':cell_config['configHash'],'role':role,'roleInstance':instance,
            'scenario':scenario,'mode':cell_config['mode'],'terminal':cell_config['terminal'],'topology':cell_config['topology'],
            'channelName':'perf-channel','meshName':None if scenario=='session-echo-only' or cell_config['mode']=='publish' or cell_config['topology']=='clientserver' else 'perf-mesh',
            'listenerEndpoint':listener,'peerEndpoint':peer,'metricsUrl':'http://127.0.0.1:'+str(p['admin']),
            'applicationTriggerUrl':'http://127.0.0.1:'+str(p['trigger'])+'/app/perf/start','source':source,'objectRole':objects,'store':store,
            'spotIds':spot_ids if objects!='None' and 'actor' not in scenario else [],'actorIds':actor_ids,'executionMode':'SpotWide' if objects=='Server' and 'actor' not in scenario else 'Framework default',
            'meshEndpoint':tcp(p['mesh']),'fanoutEndpoint':tcp(p['fanout']),'peerEndpoints':peers,'workload':cell_config['workload'],
            'diagnostics':{'level':'Normal','flowFile':str(owned.cell/'logs'/('message-flow-'+role+'-'+str(instance)+'.log'))} if args.operation=='diagnostic' else None,
            'provenance':{'comparisonKey':cell_config['comparisonKey'],'environmentFile':str(args.output/'env.json'),'buildMode':'Release',
                'loadedArtifactsFile':'loaded-artifacts.json','processKey':'server-'+role+'-'+str(instance),'source':source,
                'commit':cell_config['commit'],'serializer':'default Framework typed JSON','listenerReservation':'OS bind(127.0.0.1,0), held until exact owner starts'}}
        filename='role-configs/'+role+'-'+str(instance)+'.json';write_json(owned.cell/filename,role_config)
        endpoints={key:tcp(p[key]) for key in ('mesh','channel','fanout') if p[key]}
        if p['stream']:endpoints['stream']=listener
        role_manifest={'role':role,'roleInstance':instance,'configFile':filename,'streamEndpoint':listener if role=='session' else None,
            'applicationTriggerUrl':role_config['applicationTriggerUrl'],'metrics':{'transport':'http','baseUrl':role_config['metricsUrl']},
            'transportEndpoints':endpoints,'spotIds':role_config['spotIds'],'actorIds':actor_ids}
        roles.append(role_manifest);role_configs.append((role_config,kind,list(filter(None,p.values()))))
    return roles,role_configs


def trigger_barrier(owned,roles,clients,identity,cell,stage):
    observations=[]
    def start_role(role):
        sent=time.monotonic_ns();ack=http_json(role['applicationTriggerUrl'],identity)
        if not ack.get('accepted'):raise RuntimeError('Application trigger rejected: '+json.dumps(ack))
        return {'participant':'server-'+role['role']+'-'+str(role['roleInstance']),'sentTicks':str(sent),'ackTicks':str(time.monotonic_ns()),'acknowledgement':ack}
    # Start receiver windows before sources issue public calls; every send/ack remains in one bound.
    receivers=[r for r in roles if not r['_source']];sources=[r for r in roles if r['_source']]
    observations.extend(parallel(receivers,start_role))
    def start_client(pair):
        index,c=pair;sent=time.monotonic_ns();ack=c.call('start',identity)
        return {'participant':'client-'+str(index),'sentTicks':str(sent),'ackTicks':str(time.monotonic_ns()),'acknowledgement':ack}
    observations.extend(parallel(sources,start_role));observations.extend(parallel(list(enumerate(clients)),start_client))
    bound=max(int(o['ackTicks']) for o in observations)-min(int(o['sentTicks']) for o in observations)
    evidence={'clockDomainId':'coordinator-'+str(os.getpid()),'clockSource':'time.monotonic_ns','observedStartSkewBoundNs':str(bound),
        'exactCrossProcessStartSkewNs':None,'nullReasons':{'/exactCrossProcessStartSkewNs':{'code':'CLOCK_DOMAIN_UNVERIFIED','reason':'Process clock epochs are not asserted to be shared.'}},'participants':observations}
    write_json(cell/'tmp'/(stage+'-start-barrier.json'),evidence)
    return bound


def collect_phase(owned,roles,clients,config,cell,phase):
    duration=config['workload']['warmupSeconds' if phase=='warmup' else 'durationSeconds']
    deadline=time.monotonic()+duration+config['workload']['settleTimeoutMs']/1000
    for c in clients:c.send('wait')
    # Observe the fixed window after issuance ends; admin snapshots are not hot-path polling.
    time.sleep(duration)
    pending=list(roles);snapshots={}
    while pending:
        owned.check()
        for role in list(pending):
            name='server-'+role['role']+'-'+str(role['roleInstance'])+'.json'
            value=get_json(role['metrics']['baseUrl']+'/perf/stats',config['workload']['adminTimeoutMs']/1000)
            snapshots[name]=value
            active=value.get('runtimeMetrics',{}).get('activeHandlers',{}).get('value')
            if value['phase']=='complete' and (active is None or active=='0'):pending.remove(role)
        if pending and time.monotonic()>deadline:raise TimeoutError('Application phase/active handler drain exceeded configured duration+settle bound')
        if pending:time.sleep(.02)
    for index,c in enumerate(clients):
        c.receive(config['workload']['adminTimeoutMs']/1000)
        snapshots['client-'+str(index)+'.json']=c.call('stats',seconds=config['workload']['adminTimeoutMs']/1000)
    # Receiver observations are collected together after every participant reached its terminal bound.
    for role in roles:
        name='server-'+role['role']+'-'+str(role['roleInstance'])+'.json'
        snapshots[name]=get_json(role['metrics']['baseUrl']+'/perf/stats',config['workload']['adminTimeoutMs']/1000)
    for name,value in snapshots.items():
        write_json(cell/('tmp/warmup-'+name if phase=='warmup' else name),value)
    if phase=='warmup':
        for name,value in snapshots.items():
            if value['metrics']['errors.harness'] or int(value['runtimeMetrics'].get('phaseDiagnosticFailures',{}).get('value','0')):
                raise RuntimeError('Warmup instrumentation/validation failure preserved: '+name)
    return snapshots


def loaded_artifacts(owned,cell,language,filename="loaded-artifacts.json",allow_exited=False):
    observations=[];mismatches=[]
    for name,p in owned.processes:
        maps_error=None
        try:
            paths=sorted({line.split()[-1] for line in Path('/proc/'+str(p.pid)+'/maps').read_text().splitlines() if '/' in line and any(token in line for token in
                ('.node','libzlink','Zlink.Framework','ZLink.Framework.Perf','System.Text.Json','libjvm','libnode'))})
        except OSError as error:
            paths=[];maps_error=str(error)
        artifacts=[{'actualLoadPath':path,'resolvedPath':str(Path(path).resolve()),'sha256':digest(path)} for path in paths if Path(path).is_file()]
        native=[item for item in artifacts if 'libzlink.so' in item['actualLoadPath']]
        connector_only=name.startswith('client-') and language in ('java','node','dotnet')
        exited=p.poll() is not None
        if any(item['sha256']!=CORE_HASH for item in native) or (not native and not connector_only and not (exited and allow_exited)):
            mismatches.append(str(p.pid))
        candidates={Path('/proc/'+str(p.pid)+'/exe').resolve()}
        for arg in p.args:
            path=Path(arg)
            if path.is_file():
                candidates.add(path.resolve())
                if path.suffix=='.dll':candidates.update(path.parent.glob('*.dll'))
                if language=='java' and path.name=='zlink-framework-perf':candidates.update((path.parent.parent/'lib').glob('*.jar'))
        if language=='node':
            for suffix in ('*.js','*.cjs','*.mjs'):candidates.update((ROOT/'framework/languages/node/perf').rglob(suffix))
            for package in (ROOT/'framework/languages/node/packages').iterdir():
                if package.is_dir():
                    for suffix in ('*.js','*.cjs','*.mjs'):candidates.update((package/'dist').rglob(suffix))
                    if (package/'package.json').is_file():candidates.add(package/'package.json')
        observed_executable=Path('/proc/'+str(p.pid)+'/exe').resolve()
        launcher_artifacts=[{'resolvedPath':str(path.resolve()),'sha256':digest(path),
            'evidence':'observedProcessExecutable' if path==observed_executable else 'declaredLauncherOrInstalledFile'}
            for path in sorted(candidates) if path.is_file()]
        core_observation={'status':'loaded','artifacts':native} if native else {'status':'NOT_APPLICABLE',
            'reason':'Public WebSocket connector uses host-language WebSocket transport; this connector-only role does not create a Framework host or a Core socket.',
            'publicDependencyEvidence':{'java':'java.net.http.WebSocket','node':'stream connector host WebSocket transport','dotnet':'System.Net.WebSockets.ClientWebSocket'}.get(language)}
        if exited and not native:
            core_observation={'status':'UNAVAILABLE','reasonCode':'PROCESS_EXITED',
                'reason':maps_error or 'Process exited before a Core library mapping could be observed.','exitCode':p.poll()}
        elif maps_error:
            core_observation={'status':'UNAVAILABLE','reasonCode':'PROCESS_EXITED' if exited else 'MAPS_UNREADABLE',
                'reason':maps_error,'exitCode':p.poll()}
        elif not native and not connector_only:
            core_observation={'status':'UNAVAILABLE','reasonCode':'NO_CORE_MAPPING',
                'reason':'No Core library was observed in this Framework host process map.','exitCode':p.poll()}
        observations.append({'process':name,'pid':p.pid,'artifacts':artifacts,'launcherArtifacts':launcher_artifacts,'coreObservation':core_observation})
    write_json(cell/filename,observations)
    if mismatches:raise InvalidSetupError('ArtifactMismatch: required observed Core digest differs or is unavailable in PIDs '+','.join(mismatches))
    return observations


def cell_run(args,language,spec,environment,manifest,redis,exact,repetition=0):
    comparison_key=hashlib.sha256(exact).hexdigest()
    scenario=spec['scenario']
    variant=spec['mode']+'-'+spec['terminal']+'-'+str(spec['topology'] or 'na')+'-s'+str(spec['spotCount'])+'-'+comparison_key[:16]
    cell_id=scenario+'/'+variant+('/repeat-'+str(repetition) if args.runs>1 else '')
    cell=args.output/language/cell_id;cell.mkdir(parents=True,exist_ok=False)
    for folder in ('logs','tmp','role-configs'):(cell/folder).mkdir()
    language_options=manifest['languages'][language].get('environment',{})
    hash_input={'comparisonKey':comparison_key,'language':language,'runtimeOptions':language_options,'launchers':manifest['languages'][language]}
    config_hash=hashlib.sha256(json.dumps(hash_input,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    config={'schemaVersion':3,'runId':args.run_id,'cellId':cell_id,'configHash':config_hash,'comparisonKey':comparison_key,'language':language,
        **{key:value for key,value in spec.items() if key!='optionalManifest'},'workload':workload(args,spec),'commit':environment['commit'],
        'diagnostics':'Normal' if args.operation=='diagnostic' else 'Off','optionalExperiment':bool(spec.get('optionalManifest')),
        'comparisonInputFile':'comparison-input.json','environmentFile':str(args.output/'env.json')}
    if 'optionalManifest' in spec:
        config['workloadManifest']=spec['optionalManifest']
        config['minDeliveryRatio']=spec['optionalManifest'].get('minDeliveryRatio')
    process_env=dict(os.environ);process_env.update(language_options)
    core=environment.get('corePackage')
    if core:
        native_dir=str(Path(core['actualLibraryPath']).parent)
        process_env['LD_LIBRARY_PATH']=native_dir+(':'+process_env['LD_LIBRARY_PATH'] if process_env.get('LD_LIBRARY_PATH') else '')
        process_env['ZLINK_LIBRARY_PATH']=core['actualLibraryPath']
    owned=OwnedProcesses(cell,process_env);clients=[];roles=[];issues=[];client_files=[];server_files=[]
    (cell/'comparison-input.json').write_bytes(exact)
    try:
        # Optional workload consumers are explicit. Unsupported controls never run a different workload.
        optional=spec.get('optionalManifest',{})
        supported={'scenario','mode','terminal','spotCount','topology','logicalStreams','connections','inflight','warmupSeconds','measuredSeconds','repetitions',
            'applicationDeadlineMs','requestTimeoutMs','correlationExpiryMs','settleTimeoutMs','minDeliveryRatio','workerTaskMillis','workerPoolSize','subscriberCount'}
        absent=set(optional)-supported
        if absent:raise UnsupportedCellError('No public application consumer for selected manifest controls: '+','.join(sorted(absent))+'; requested manifest preserved')
        roles,role_configs=make_roles(args,config,owned,redis)
        server_files=['server-'+r['role']+'-'+str(r['roleInstance'])+'.json' for r in roles]
        cs=spec['streamTransport'] is not None
        client_files=['client-'+str(i)+'.json' for i in range(config['workload']['clientCount'])] if cs else []
        config['metricOwners']=client_files if cs else [server_files[i] for i,(rc,_,_) in enumerate(role_configs) if rc['source']]
        config['receiverFiles']=[server_files[i] for i,(rc,_,_) in enumerate(role_configs) if not rc['source']]
        write_json(cell/'config.json',config)
        endpoint_manifest={'runId':args.run_id,'cellId':cell_id,'configHash':config_hash,'scenario':scenario,'mode':spec['mode'],'workload':config['workload'],'roles':roles,
            'provenance':{'comparisonKey':comparison_key,'environmentFile':str(args.output/'env.json'),'loadedArtifactsFile':'loaded-artifacts.json',
                'commit':environment['commit'],'serializer':'default Framework typed JSON','buildMode':'Release'}}
        write_json(cell/'endpoints.json',endpoint_manifest)
        for role,(rc,kind,ports) in zip(roles,role_configs):
            owned.start('server-'+rc['role']+'-'+str(rc['roleInstance']),launch_command(manifest,language,kind)+['--config',str(cell/role['configFile'])],ports)
            role['_source']=rc['source']
        wait_ready(owned,roles,False,cell,'infrastructure',DEFAULTS['startupTimeoutMs']/1000)
        loaded_artifacts(owned,cell,language,filename='tmp/infrastructure-loaded-artifacts.json')
        setup_deadline=time.monotonic()+config['workload']['setupTimeoutMs']/1000
        prepare={'runId':args.run_id,'cellId':cell_id,'resetSeq':'0','phase':'setup'}
        prepare_acks=parallel([r for r in roles if r['_source']],lambda role:http_json(role['applicationTriggerUrl'].rsplit('/',1)[0]+'/prepare',prepare,
            max(.001,setup_deadline-time.monotonic())))
        write_json(cell/'tmp/setup-prepare.json',prepare_acks)
        for index in range(len(client_files)):
            p=owned.start('client-'+str(index),launch_command(manifest,language,'client')+['--endpoint-config',str(cell/'endpoints.json'),'--client-index',str(index)],client=True)
            c=ClientControl(p,cell/'logs'/('client-'+str(index)+'-control.log'),cell/'tmp'/('client-'+str(index)+'-setup.json'));clients.append(c);owned.controls.append(c)
        prepared=parallel(list(enumerate(clients)),lambda pair:pair[1].receive(max(.001,setup_deadline-time.monotonic()),prepared=True))
        if cs:
            requested=sum(int(value['snapshot']['metrics']['connections.requested']) for value in prepared)
            connected=sum(int(value['snapshot']['metrics']['connections.connected']) for value in prepared)
            if requested!=config['workload']['connections'] or connected*100<requested*99:raise InvalidSetupError('CS connector setup below 99% or requested split differs')
            config['preparedConnections']={'requested':str(requested),'connected':str(connected),'comparisonCountMatched':connected==requested}
            if connected!=requested:issues.append({'code':'ConnectionCountMismatch','message':'Prepared connector count does not equal comparison input; 99% setup threshold passed.', 'sourceFile':'tmp/client-*-setup.json'})
        wait_ready(owned,roles,True,cell,'global-probe',max(.001,setup_deadline-time.monotonic()))
        loaded_artifacts(owned,cell,language)
        if args.prepare_only:
            issues.append({'code':'PrepareOnly','message':'Setup smoke selected; measured phase not started.','sourceFile':'tmp/setup-prepare.json'})
            for role in roles:write_json(cell/('server-'+role['role']+'-'+str(role['roleInstance'])+'.json'),get_json(role['metrics']['baseUrl']+'/perf/stats',config['workload']['adminTimeoutMs']/1000))
            for index,c in enumerate(clients):write_json(cell/('client-'+str(index)+'.json'),c.call('stats',seconds=config['workload']['adminTimeoutMs']/1000))
        else:
            for phase,reset_seq in (('warmup','0'),('measured','1')):
                if phase=='measured':
                    request={'runId':args.run_id,'cellId':cell_id,'resetSeq':reset_seq}
                    acks=parallel(roles,lambda role:http_json(role['metrics']['baseUrl']+'/perf/reset',request))
                    acks+=parallel(clients,lambda c:c.call('reset',request))
                    write_json(cell/'tmp/reset-barrier.json',acks)
                    if any(not ack['ok'] or ack['resetSeq']!='1' for ack in acks):raise RuntimeError('Reset acknowledgement barrier did not converge')
                    wait_ready(owned,roles,True,cell,'measured',config['workload']['adminTimeoutMs']/1000)
                trigger={'runId':args.run_id,'cellId':cell_id,'resetSeq':reset_seq,'phase':phase}
                bound=trigger_barrier(owned,roles,clients,trigger,cell,phase)
                if bound>DEFAULTS['maxStartSkewMs']*1_000_000:
                    issues.append({'code':'StartSkewExceeded','message':'Coordinator start skew bound '+str(bound)+'ns exceeds '+str(DEFAULTS['maxStartSkewMs'])+'ms.','sourceFile':'tmp/'+phase+'-start-barrier.json'})
                snapshots=collect_phase(owned,roles,clients,config,cell,phase)
                if phase=='warmup':
                    for name,value in snapshots.items():
                        if value['metrics']['errors.byKind'] or value['metrics']['errors.language']:
                            issues.append({'code':'WarmupPublicFailure','message':'Warmup public terminals drained; fixed measured phase continues.',
                                'sourceFile':'tmp/warmup-'+name,'errorCounts':{'byKind':value['metrics']['errors.byKind'],'language':value['metrics']['errors.language']}})
    except (Exception,KeyboardInterrupt) as error:
        code='PublicContractMismatch' if isinstance(error,UnsupportedCellError) else 'ArtifactMismatch' if 'ArtifactMismatch' in str(error) else 'InvalidSetup' if isinstance(error,InvalidSetupError) else 'CollectionFailure'
        source='logs/'
        if isinstance(error,urllib.error.HTTPError):
            source='tmp/http-error.json'
            write_json(cell/source,{'status':error.code,'url':error.url,'body':error.read().decode('utf-8',errors='replace')})
        issues.append({'code':code,'message':type(error).__name__+': '+str(error),'sourceFile':source})
        write_json(cell/'failure.json',issues)
        if owned.processes and not (cell/'loaded-artifacts.json').exists():
            try:loaded_artifacts(owned,cell,language,allow_exited=True)
            except (OSError,ValueError,InvalidSetupError) as artifact_error:
                issues.append({'code':'ArtifactMismatch' if 'ArtifactMismatch' in str(artifact_error) else 'CollectionFailure',
                    'message':str(artifact_error),'sourceFile':'loaded-artifacts.json'})
        if not (cell/'config.json').exists():write_json(cell/'config.json',config)
        if not (cell/'endpoints.json').exists():write_json(cell/'endpoints.json',{'roles':[],'reason':'Selected controls have no public application consumer.'})
        for role in roles:
            path=cell/('server-'+role['role']+'-'+str(role['roleInstance'])+'.json')
            if not path.exists():
                try:write_json(path,get_json(role['metrics']['baseUrl']+'/perf/stats',timeout=config['workload']['adminTimeoutMs']/1000))
                except (OSError,ValueError,TimeoutError) as e:issues.append({'code':'CollectionFailure','message':str(e),'sourceFile':path.name})
        for index,c in enumerate(clients):
            path=cell/('client-'+str(index)+'.json')
            if not path.exists():
                try:write_json(path,c.call('stats',seconds=config['workload']['adminTimeoutMs']/1000))
                except (OSError,ValueError,TimeoutError,RuntimeError) as e:
                    issues.append({'code':'CollectionFailure','message':str(e),'sourceFile':path.name})
                    if c.prepared_path is not None and c.prepared_path.exists():
                        prepared=json.loads(c.prepared_path.read_text())
                        if isinstance(prepared.get('snapshot'),dict):write_json(path,prepared['snapshot'])
    finally:
        try:owned.cleanup()
        except (OSError,RuntimeError) as e:issues.append({'code':'CollectionFailure','message':'Shutdown: '+str(e),'sourceFile':'cleanup.json'})
    result=aggregate(cell,config,client_files,server_files,issues)
    validate_result(result)
    print('language='+language+' cell='+cell_id+' status='+result['status']+' result='+str(cell/'result.json'),flush=True)
    return result,cell


def resolve_runtime_manifest(manifest, languages):
    for language in languages:
        entry=manifest['languages'][language]
        configured=entry.setdefault('environment',{})
        for key,value in list(configured.items()):
            configured[key]=os.path.expandvars(value)
            if '$' in configured[key]:raise InvalidSetupError('Unresolved runtime environment variable: '+key)
        if language=='java':
            preferred=configured.get('JAVA_HOME') or os.environ.get('JAVA_HOME')
            candidates=[Path(preferred)] if preferred else sorted((Path.home()/'.cache/jdks').glob('jdk-25*'),reverse=True)
            chosen=None
            for candidate in candidates:
                executable=candidate/'bin/java'
                if not executable.is_file():continue
                result=subprocess.run([str(executable),'-version'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
                import re
                if result.returncode==0 and re.search(r'version "25[.]',result.stdout):
                    chosen=candidate.resolve();entry['runtimeVersionEvidence']={'executable':str(executable.resolve()),'output':result.stdout};break
            if chosen is None:raise InvalidSetupError('Java perf requires public JDK25; configure JAVA_HOME or install a JDK25 in ~/.cache/jdks')
            configured['JAVA_HOME']=str(chosen)
            configured['PATH']=str(chosen/'bin')+os.pathsep+configured.get('PATH',os.environ.get('PATH',''))
    return manifest


def main(argv=None):
    args=options(sys.argv[1:] if argv is None else argv)
    manifest=json.loads(args.executables_manifest.read_text())
    if not manifest.get('corePackagePrefix'):
        manifest['corePackagePrefix']=os.environ.get('ZLINK_CORE_PACKAGE_PREFIX',str(Path.home()/'.cache/zlink/core/0.18.0/linux-x64'))
    if manifest.get('requiredCoreSha256',CORE_HASH)!=CORE_HASH:raise InvalidSetupError('Manifest Core digest differs from the fixed comparison package')
    resolve_runtime_manifest(manifest,args.selected_languages)
    environment=collect(manifest)
    if 'corePackage' not in environment:raise InvalidSetupError('A public fixed Core package prefix is required')
    specs=selected_cells(args)
    if args.runs!=1:raise InvalidSetupError('Every selected cell uses runs=1; repeated runs are not authorized')
    args.output.mkdir(parents=True,exist_ok=False)
    write_json(args.output/'env.json',environment);write_json(args.output/'executables-manifest.json',manifest)
    build_languages(args,manifest)
    inputs=[comparison_input(args,spec,environment,manifest['redis']) for spec in specs]
    redis=RunRedis(args.output,manifest['redis']);results=[]
    try:
        for language in args.selected_languages:
            for repetition in range(args.runs):
                for index,(spec,exact) in enumerate(zip(specs,inputs)):
                    if results:time.sleep(DEFAULTS['cooldownMs']/1000)
                    result,cell=cell_run(args,language,spec,environment,manifest,redis,exact,repetition)
                    results.append({'cellId':result['cellId'],'language':language,'resultFile':str((cell/'result.json').relative_to(args.output)),
                        'status':result['status'],'baselineEligible':result['baselineEligible'],'comparisonKey':result['comparisonKey']})
    finally:redis.close()
    write_json(args.output/'index.json',{'schemaVersion':3,'runId':args.run_id,'cells':results})
    write_json(args.output/'summary.json',{'schemaVersion':3,'runId':args.run_id,'cells':results,'counts':{status:sum(r['status']==status for r in results) for status in ('valid','invalid','failed','unsupported')}})
    return 0 if all(r['status']=='valid' for r in results) else 1

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,InvalidSetupError,UnsupportedCellError) as error:raise SystemExit(type(error).__name__+': '+str(error))
