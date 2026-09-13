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
                self.assertEqual(c['workload']['connections'],10000);self.assertIsNone(c['workload']['logicalStreams'])
                self.assertEqual(sum(p[0]=='session' for p in plan),1)
            else:
                self.assertEqual(sum(p[2] for p in plan),1)
            if cell['mode']=='publish':self.assertEqual(sum(p[0]=='subscriber' for p in plan),8)
            self.assertEqual((c['workload']['warmupSeconds'],c['workload']['durationSeconds']),(2,5))
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

if __name__=='__main__':unittest.main()
