import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from runner import options, selected_cells, workload, role_specs, make_roles, comparison_input, launch_command, OwnedProcesses, MATRIX
from environment import CORE_HASH


class RunnerTests(unittest.TestCase):
    def args(self,*extra):return options(['matrix','--output','/tmp/zlink-336-contract-unused',*extra])
    def test_default_84_cells_and_role_plans(self):
        args=self.args();cells=selected_cells(args)
        self.assertEqual(len(cells),21);self.assertEqual(len(args.selected_languages)*len(cells),84)
        for cell in cells:
            c={**cell,'workload':workload(args,cell)};plan=role_specs(c)
            self.assertTrue(plan)
            if cell['streamTransport']:
                self.assertEqual(c['workload']['connections'],1000);self.assertIsNone(c['workload']['logicalStreams'])
                self.assertEqual(sum(p[0]=='session' for p in plan),1)
            else:
                self.assertEqual(sum(p[2] for p in plan),1)
            if cell['mode']=='publish':self.assertEqual(sum(p[0]=='subscriber' for p in plan),8)
            self.assertEqual((c['workload']['warmupSeconds'],c['workload']['durationSeconds']),(2,5))
    def test_ccu_cap_applies_to_cli_and_both_manifest_owners_without_inflight_product(self):
        from runner import InvalidSetupError
        from validator import validate_schema
        validate_schema({'connections':1000,'logicalStreams':1000,'inflight':2},'CcuWorkload')
        for key in ('connections','logical-streams'):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):self.args('--'+key,'1001')
        for key in ('connections','logicalStreams'):
            with self.assertRaises(ValueError):validate_schema({key:1001},'CcuWorkload')
        for spec in MATRIX['cells']:
            configured=workload(self.args('--inflight','2'),spec)
            if spec['scenario']=='spot-worker-offload-echo':self.assertEqual(configured['logicalStreams'],8)
            elif spec['streamTransport']:self.assertEqual(configured['connections'],1000)
            else:self.assertEqual(configured['logicalStreams'],1000)
        self.assertEqual(workload(self.args('--logical-streams','1000','--inflight','2'),MATRIX['cells'][19])['inflight'],2)
        for key in ('connections','logicalStreams'):
            with self.assertRaises(InvalidSetupError):
                from runner import validate_ccu
                validate_ccu({key:1000.0})
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'manifest.json'
            for key in ('connections','logicalStreams'):
                for switch,data in (('--comparison-config',{'cell':MATRIX['cells'][19],'workload':{key:1001}}),
                                    ('--comparison-config',{'cell':{**MATRIX['cells'][19],key:1001}}),
                                    ('--workload-config',{**MATRIX['cells'][19],key:1001})):
                    p.write_text(json.dumps(data))
                    with self.subTest(key=key,switch=switch),self.assertRaises(InvalidSetupError):selected_cells(self.args(switch,str(p)))

    def test_sealed_receivers_use_one_final_admin_snapshot_after_app_observation_bound(self):
        from runner import collect_phase
        from unittest.mock import patch
        for mode in ('send','publish'):
            for phase,duration in (('warmup',2),('measured',5)):
                with self.subTest(mode=mode,phase=phase),tempfile.TemporaryDirectory() as folder:
                    cell=Path(folder);(cell/'tmp').mkdir()
                    roles=[{'role':'subscriber','roleInstance':i,'metrics':{'baseUrl':'http://receiver/'+str(i)}} for i in range(2)]
                    snapshot={'phase':'complete','window':{'startTicks':'-100','endTicks':str(duration*1_000_000_000-100),'measuredSeconds':duration},
                              'runtimeMetrics':{'activeHandlers':{'value':'0'}},'metrics':{'errors.harness':{}}}
                    owned=SimpleNamespace(check=lambda:None)
                    config={'mode':mode,'workload':{'warmupSeconds':2,'durationSeconds':5,'settleTimeoutMs':5000,'adminTimeoutMs':5000}}
                    with patch('runner.time.sleep') as sleep,patch('runner.get_json',return_value=snapshot) as query:
                        self.assertEqual(len(collect_phase(owned,roles,[],config,cell,phase)),2)
                    sleep.assert_called_once_with(duration+5)
                    self.assertEqual(query.call_count,2)
                    self.assertTrue(all(call.args[1]==5 for call in query.call_args_list))
                    for bad in ({**snapshot,'phase':'measured'},{**snapshot,'window':{}},
                                {**snapshot,'window':{**snapshot['window'],'endTicks':'100'}},
                                {**snapshot,'runtimeMetrics':{'activeHandlers':{'value':None}}}):
                        with patch('runner.time.sleep'),patch('runner.get_json',return_value=bad):
                            with self.assertRaises(TimeoutError):collect_phase(owned,roles,[],config,cell,phase)

    def test_default_single_selection(self):
        args=options(['single','--scenario','channel-echo-only','--output','/tmp/zlink-336-contract-unused'])
        self.assertEqual(selected_cells(args)[0]['topology'],'routemesh')
    def test_cli_rejects_unknown_or_duplicate_consumer(self):
        invalid=[['single'],['matrix','--duration-seconds','nan'],['matrix','--inflight','0'],['matrix','--codec','raw'],['matrix','--runs','2']]
        with contextlib.redirect_stderr(io.StringIO()):
            for bad in invalid:
                with self.subTest(argv=bad), self.assertRaises(SystemExit):options(bad)
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'comparison.json';p.write_text('{}')
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):options(['matrix','--comparison-config',str(p),'--inflight','2'])
    def test_comparison_exact_bytes_and_actual_workload_consumer(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'comparison.json';data={'cell':MATRIX['cells'][19],'workload':{'inflight':3,'logicalStreams':17}}
            exact=json.dumps(data,indent=1).encode();p.write_bytes(exact)
            args=self.args('--comparison-config',str(p));cell=selected_cells(args)[0]
            self.assertEqual(workload(args,cell)['logicalStreams'],17)
            self.assertEqual(workload(args,cell)['inflight'],3)
            self.assertEqual(comparison_input(args,cell,{},{}),exact)
    def test_comparison_excludes_language_ports_run_id(self):
        args=self.args();cell=MATRIX['cells'][19]
        env={'cpuAffinity':[0,1],'cpuQuota':'max 100000','cpuset':'0-1','memoryLimit':'max','effectiveProcessorCount':2}
        a=comparison_input(args,cell,env,{'image':'redis:7.4-alpine'})
        args.run_id='different';args.language='java'
        self.assertEqual(comparison_input(args,cell,env,{'image':'redis:7.4-alpine'}),a)
        self.assertIn(CORE_HASH.encode(),a)
    def test_all_role_configs_shape_and_store_namespaces(self):
        class Redis:
            calls=0
            def ensure(self):self.calls+=1;return {'endpoint':'127.0.0.1:6380'}
        args=self.args();redis=Redis()
        for index,spec in enumerate(MATRIX['cells']):
            with tempfile.TemporaryDirectory() as folder:
                cell=Path(folder);(cell/'role-configs').mkdir();owned=OwnedProcesses(cell,{})
                config={**spec,'workload':workload(args,spec),'cellId':'c'+str(index),'configHash':'hash','comparisonKey':'common','commit':'head'}
                roles,configs=make_roles(args,config,owned,redis)
                self.assertEqual(len(roles),len(configs))
                for manifest,(c,kind,ports) in zip(roles,configs):
                    self.assertNotEqual(c['metricsUrl'],c['applicationTriggerUrl'].rsplit('/',3)[0])
                    self.assertEqual(c['mode'],spec['mode']);self.assertEqual(c['terminal'],spec['terminal'])
                    self.assertEqual(set(c),{'runId','cellId','configHash','role','roleInstance','scenario','mode','terminal','topology','channelName','meshName',
                        'listenerEndpoint','peerEndpoint','metricsUrl','applicationTriggerUrl','source','objectRole','store','spotIds','actorIds','executionMode',
                        'meshEndpoint','fanoutEndpoint','peerEndpoints','workload','diagnostics','provenance'})
                    if spec['scenario'] in ('session-echo-only','channel-echo-only'):self.assertIsNone(c['store'])
                    else:
                        self.assertEqual(c['store']['provider'],'redis')
                        self.assertEqual(c['peerEndpoints'],[])
                        self.assertIsNone(c['peerEndpoint'])
                if spec['scenario']=='channel-echo-only' and spec['topology']=='routemesh':self.assertTrue(configs[-1][0]['peerEndpoint'])
                for s in owned.reservations:s.close()
    def test_pubsub_subscriber_count_consumed(self):
        args=self.args('--subscriber-count','3');spec=next(c for c in MATRIX['cells'] if c['mode']=='publish')
        c={**spec,'workload':workload(args,spec)}
        self.assertEqual(sum(p[0]=='subscriber' for p in role_specs(c)),3)
    def test_supplied_resource_plan_requires_actual_matching_consumer(self):
        from runner import InvalidSetupError
        env={'cpuAffinity':[0,1],'cpuQuota':'max 100000','cpuset':'0-1','memoryLimit':'max','effectiveProcessorCount':2}
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'comparison.json';data={'cell':MATRIX['cells'][19],'resourcePlan':{'affinity':[0,1],'requiredCoreSha256':CORE_HASH}}
            p.write_text(json.dumps(data));args=self.args('--comparison-config',str(p))
            self.assertEqual(comparison_input(args,MATRIX['cells'][19],env,{}),p.read_bytes())
            for key,value in [('affinity',[2]),('cpuQuota','100000 100000'),('unownedTuning',True)]:
                broken=json.loads(json.dumps(data));broken['resourcePlan'][key]=value;p.write_text(json.dumps(broken))
                with self.assertRaises(InvalidSetupError):comparison_input(args,MATRIX['cells'][19],env,{})

    def test_exact_launchers(self):
        manifest=json.loads((Path(__file__).parent/'launchers.json').read_text())
        self.assertIn('main.js',launch_command(manifest,'node','client')[-1])
        self.assertTrue(launch_command(manifest,'java','client')[0].endswith('/zlink-framework-perf'))
        self.assertTrue(launch_command(manifest,'cpp','client')[0].endswith('/zlink_framework_perf'))

    def test_server_prepare_completes_targets_before_sources_under_one_deadline(self):
        from unittest.mock import patch
        from runner import prepare_server_roles
        roles=[{'_source':True,'applicationTriggerUrl':'http://source/app/perf/start'},
               {'_source':False,'applicationTriggerUrl':'http://target-1/app/perf/start'},
               {'_source':False,'applicationTriggerUrl':'http://target-2/app/perf/start'}]
        request={'runId':'fixture','cellId':'fixture','phase':'setup','resetSeq':'0'}
        completed=set();timeouts=[]
        def prepare(url,body,timeout):
            self.assertEqual(body,request)
            if url.startswith('http://source/'):
                self.assertEqual(completed,{'http://target-1/app/perf/prepare','http://target-2/app/perf/prepare'})
            completed.add(url);timeouts.append(timeout)
            return {'ok':True,'url':url}
        with tempfile.TemporaryDirectory() as folder:
            cell=Path(folder);(cell/'tmp').mkdir()
            with patch('runner.http_json',side_effect=prepare),patch('runner.time.monotonic',side_effect=[10,14,22]):
                prepare_server_roles(roles,request,40,cell)
            self.assertEqual(sorted(timeouts),[18,26,30])
            saved=json.loads((cell/'tmp/setup-prepare.json').read_text())
            self.assertEqual(len(saved),3)
            self.assertEqual(saved[-1]['url'],'http://source/app/perf/prepare')

    def test_target_prepare_failure_preserves_original_exception_and_never_starts_sources(self):
        from unittest.mock import patch
        from runner import prepare_server_roles
        roles=[{'_source':False,'applicationTriggerUrl':'http://target/app/perf/start'},
               {'_source':True,'applicationTriggerUrl':'http://source/app/perf/start'}]
        error=RuntimeError('original public target preparation failure')
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder)/'tmp').mkdir()
            with patch('runner.http_json',side_effect=error) as request,patch('runner.time.monotonic',return_value=10):
                with self.assertRaises(RuntimeError) as raised:
                    prepare_server_roles(roles,{},40,Path(folder))
            self.assertIs(raised.exception,error)
            self.assertEqual(request.call_count,1)
            self.assertEqual(request.call_args.args[0],'http://target/app/perf/prepare')
            self.assertEqual(json.loads((Path(folder)/'tmp/setup-prepare.json').read_text()),[])

    def test_source_prepare_failure_saves_target_ack_without_replacing_original_error(self):
        from unittest.mock import patch
        from runner import prepare_server_roles
        roles=[{'_source':False,'applicationTriggerUrl':'http://target/app/perf/start'},
               {'_source':True,'applicationTriggerUrl':'http://source/app/perf/start'}]
        target_ack={'ok':True,'target':'actual target preparation'}
        error=RuntimeError('original public source preparation failure')
        for storage_fails in (False,True):
            with self.subTest(storage_fails=storage_fails),tempfile.TemporaryDirectory() as folder:
                cell=Path(folder);(cell/'tmp').mkdir()
                with contextlib.ExitStack() as stack:
                    stack.enter_context(patch('runner.http_json',side_effect=[target_ack,error]))
                    stack.enter_context(patch('runner.time.monotonic',return_value=10))
                    if storage_fails:stack.enter_context(patch('runner.write_json',side_effect=OSError('artifact storage failure')))
                    with self.assertRaises(RuntimeError) as raised:prepare_server_roles(roles,{},40,cell)
                self.assertIs(raised.exception,error)
                if not storage_fails:self.assertEqual(json.loads((cell/'tmp/setup-prepare.json').read_text()),[target_ack])

    def test_http_failure_preserves_response_body_without_another_request(self):
        import urllib.error
        from unittest.mock import patch
        from runner import cell_run
        with tempfile.TemporaryDirectory() as folder:
            args=options(['single','--scenario','channel-echo-only','--language','node','--output',str(Path(folder)/'run')])
            spec=selected_cells(args)[0]
            body='{"errorType":"ZLinkConfigurationException","message":"public setup failure"}'
            error=urllib.error.HTTPError('http://127.0.0.1/app/perf/prepare',400,'Bad Request',{},io.BytesIO(body.encode()))
            manifest={'languages':{'node':{'server':['node']}}}
            with patch('runner.make_roles',side_effect=error) as request,contextlib.redirect_stdout(io.StringIO()):
                result,cell=cell_run(args,'node',spec,{'commit':'fixture'},manifest,None,b'{}')
            self.assertEqual(request.call_count,1)
            self.assertEqual(json.loads((cell/'tmp/http-error.json').read_text()),{'status':400,'url':error.url,'body':body})
            self.assertEqual(result['status'],'failed')
            self.assertEqual(json.loads((cell/'failure.json').read_text())[0]['sourceFile'],'tmp/http-error.json')

class ClientControlTests(unittest.TestCase):
    def test_failed_prepared_original_is_saved_before_rejection(self):
        from runner import ClientControl, InvalidSetupError
        with tempfile.TemporaryDirectory() as folder:
            prepared=Path(folder)/'prepared.json'
            control=ClientControl(SimpleNamespace(),Path(folder)/'control.log',prepared)
            envelope={'type':'prepared','ok':False,'snapshot':{'phase':'setup','completedSetupEvidence':[{'clientId':7}]}}
            control.buffer=(json.dumps(envelope)+'\n').encode()
            try:
                with self.assertRaises(InvalidSetupError):control.receive(.1,prepared=True)
                self.assertEqual(json.loads(prepared.read_text()),envelope)
            finally:control.log.close()

    def test_late_prepared_and_stats_use_one_reader_and_one_command(self):
        import subprocess,sys
        from runner import ClientControl
        script='''import json,sys
command=json.loads(sys.stdin.readline())
assert command=={'command':'stats'}
print(json.dumps({'type':'prepared','ok':False,'snapshot':{'phase':'setup'}}),flush=True)
print(json.dumps({'ok':True,'response':{'phase':'setup','actualStats':True}}),flush=True)
'''
        with tempfile.TemporaryDirectory() as folder:
            process=subprocess.Popen([sys.executable,'-c',script],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
            prepared=Path(folder)/'prepared.json';control=ClientControl(process,Path(folder)/'control.log',prepared)
            try:
                self.assertEqual(control.call('stats',seconds=1),{'phase':'setup','actualStats':True})
                self.assertFalse(json.loads(prepared.read_text())['ok'])
                self.assertEqual(process.wait(timeout=1),0)
            finally:
                process.stdin.close();process.stdout.close();control.log.close()
                if process.poll() is None:process.kill();process.wait()

    def test_late_prepared_does_not_restart_admin_deadline(self):
        import subprocess,sys,time
        from runner import ClientControl
        script='''import json,sys,time
json.loads(sys.stdin.readline())
time.sleep(.10)
print(json.dumps({'type':'prepared','ok':False,'snapshot':{'phase':'setup'}}),flush=True)
time.sleep(.20)
print(json.dumps({'ok':True,'response':{'phase':'setup'}}),flush=True)
'''
        with tempfile.TemporaryDirectory() as folder:
            process=subprocess.Popen([sys.executable,'-c',script],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
            prepared=Path(folder)/'prepared.json';control=ClientControl(process,Path(folder)/'control.log',prepared)
            try:
                started=time.monotonic()
                with self.assertRaises(TimeoutError):control.call('stats',seconds=.20)
                self.assertLess(time.monotonic()-started,.28)
                self.assertTrue(prepared.exists())
            finally:
                process.kill();process.wait();process.stdin.close();process.stdout.close();control.log.close()

class ArtifactPreservationTests(unittest.TestCase):
    def test_exited_empty_process_map_is_unavailable_without_pin_failure(self):
        import subprocess,sys
        from unittest.mock import patch
        from runner import loaded_artifacts
        with tempfile.TemporaryDirectory() as folder:
            process=subprocess.Popen([sys.executable,'-c','pass']);process.wait(timeout=2)
            owned=SimpleNamespace(processes=[('server-framework-0',process)])
            original_read=Path.read_text
            with patch.object(Path,'read_text',lambda path,*a,**kw:'' if path.name=='maps' else original_read(path,*a,**kw)):
                loaded_artifacts(owned,Path(folder),'cpp',allow_exited=True)
            observation=json.loads((Path(folder)/'loaded-artifacts.json').read_text())[0]
            self.assertEqual(observation['coreObservation']['status'],'UNAVAILABLE')
            self.assertEqual(observation['coreObservation']['reasonCode'],'PROCESS_EXITED')

    def test_exited_host_is_unavailable_and_never_core_not_applicable(self):
        import subprocess,sys
        from runner import loaded_artifacts
        with tempfile.TemporaryDirectory() as folder:
            process=subprocess.Popen([sys.executable,'-c','pass']);process.wait(timeout=2)
            owned=SimpleNamespace(processes=[('server-framework-0',process)])
            loaded_artifacts(owned,Path(folder),'cpp',allow_exited=True)
            observation=json.loads((Path(folder)/'loaded-artifacts.json').read_text())[0]
            self.assertEqual(observation['coreObservation']['status'],'UNAVAILABLE')
            self.assertEqual(observation['coreObservation']['reasonCode'],'PROCESS_EXITED')
            self.assertEqual(observation['coreObservation']['exitCode'],0)
            self.assertTrue(observation['launcherArtifacts'])
            self.assertTrue(all(a['evidence']=='declaredLauncherOrInstalledFile' for a in observation['launcherArtifacts']))

    def test_missing_mandatory_core_observation_is_written_before_rejection(self):
        import subprocess,sys
        from runner import loaded_artifacts,InvalidSetupError
        with tempfile.TemporaryDirectory() as folder:
            process=subprocess.Popen([sys.executable,'-c','import time;time.sleep(5)'])
            owned=SimpleNamespace(processes=[('server-framework-0',process)])
            try:
                with self.assertRaises(InvalidSetupError):loaded_artifacts(owned,Path(folder),'cpp')
                observation=json.loads((Path(folder)/'loaded-artifacts.json').read_text())[0]
                self.assertEqual(observation['coreObservation']['status'],'UNAVAILABLE')
                self.assertEqual(observation['coreObservation']['reasonCode'],'NO_CORE_MAPPING')
                self.assertTrue(any(a['evidence']=='observedProcessExecutable' for a in observation['launcherArtifacts']))
            finally:process.kill();process.wait()


class CancellationTests(unittest.TestCase):
    def test_sigterm_and_keyboard_interrupt_preserve_original_and_cleanup_without_next_cell(self):
        import subprocess,sys,os
        script = r"""
import json,os,signal,sys
from pathlib import Path
from unittest.mock import patch
import runner
output=Path(sys.argv[1]); manifest=output.parent/'launchers.json'
manifest.write_text(json.dumps({'corePackagePrefix':'fixture','languages':{'node':{'server':['node']}},'redis':{}}))
class Redis:
    def __init__(self,*args):pass
    def close(self):(output/'redis-cleanup.json').write_text(json.dumps({'exactOwnedCleanupInvoked':True}))
def interrupted(args,config,owned,redis):
    with (output/'cell-starts.txt').open('a') as stream:stream.write('start\n')
    child=owned.start('fixture-owned-child',[sys.executable,'-c','import time; time.sleep(30)'])
    if sys.argv[2] in ('sigterm','sigint'):os.kill(os.getpid(),signal.SIGTERM if sys.argv[2]=='sigterm' else signal.SIGINT)
    raise KeyboardInterrupt('fixture cancellation')
try:
    with patch('runner.collect',return_value={'commit':'fixture','corePackage':{}}),patch('runner.build_languages'),patch('runner.comparison_input',return_value=b'{}'),patch('runner.resolve_runtime_manifest'),patch('runner.RunRedis',Redis),patch('runner.make_roles',side_effect=interrupted):
        runner.main(['matrix','--language','node','--skip-build','--executables-manifest',str(manifest),'--output',str(output)])
except KeyboardInterrupt:sys.exit(130)
"""
        for cause in ('sigterm','sigint','keyboard'):
            with self.subTest(cause=cause),tempfile.TemporaryDirectory() as folder:
                output=Path(folder)/'run'
                completed=subprocess.run([sys.executable,'-c',script,str(output),cause],cwd=Path(__file__).parent,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=15)
                self.assertEqual(completed.returncode,130,completed.stderr)
                self.assertEqual(len((output/'cell-starts.txt').read_text().splitlines()),1)
                self.assertEqual(json.loads((output/'index.json').read_text())['cells'],[])
                self.assertFalse(json.loads((output/'abort.json').read_text())['finalComparisonComplete'])
                self.assertTrue(json.loads((output/'redis-cleanup.json').read_text())['exactOwnedCleanupInvoked'])
                cell=next(output.glob('node/*/*/cleanup.json')).parent
                self.assertTrue((cell/'failure.json').exists())
                cleanup=json.loads((cell/'cleanup.json').read_text())['ownedProcesses']
                self.assertEqual(len(cleanup),1);self.assertTrue(cleanup[0]['reaped'])
                with self.assertRaises(ProcessLookupError):os.kill(cleanup[0]['pid'],0)

if __name__=='__main__':unittest.main()
