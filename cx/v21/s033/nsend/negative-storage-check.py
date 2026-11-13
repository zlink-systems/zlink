import pathlib,subprocess,json
root=pathlib.Path('/root/worktree/zlink-nsend/framework/languages/node')
p=root/'packages/framework/dist/runtime/backend/node/node-raw-binding-port.js'
original=p.read_text()
needle='retained[index] = runtime_message_1.ZLinkBufferMessage.retainPayload(part);'
if needle not in original:
 matches=[x.strip() for x in original.splitlines() if 'retained[index]' in x]
 raise SystemExit('negative mutation anchor missing: '+str(matches))
try:
 p.write_text(original.replace(needle,'retained[index] = Buffer.alloc(0);'))
 r=subprocess.run(['node','--test','--test-name-pattern=with 4KiB payloads','test/contract/node-raw-receive-storage-cost.test.js'],cwd=root,capture_output=True,text=True)
 (pathlib.Path(__file__).parent/'negative-storage-check.log').write_text(r.stdout+r.stderr)
 assert r.returncode!=0 and 'AssertionError' in r.stdout, 'corrupted storage was not detected'
 print('NEGATIVE CHECK: corrupt retained bytes rejected by 4KiB lifetime contract')
finally:p.write_text(original)
try:
 baseline=pathlib.Path(__file__).parent/'baseline-code/framework/languages/node/packages/framework/dist/runtime/backend/node/node-raw-binding-port.js'
 p.write_text(baseline.read_text())
 subprocess.run(['node','--test','test/contract/node-raw-receive-storage-cost.test.js'],cwd=root,check=True)
 print('BASELINE CHECK: the unchanged runtime also passes the strengthened lifetime contract')
finally:p.write_text(original)
