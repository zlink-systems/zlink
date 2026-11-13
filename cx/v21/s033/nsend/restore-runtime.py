import pathlib,json,hashlib,shutil
J=pathlib.Path(__file__).parent;R=pathlib.Path('/root/worktree/zlink-nsend');B=J/'baseline-code';P=J/'prototype-code'
source=pathlib.Path('framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts')
assert (R/source).read_bytes() in [(P/source).read_bytes(),(B/source).read_bytes()],'runtime source changed outside this prototype'
manifest=json.loads((J/'variant-manifest.json').read_text());dist=pathlib.Path('framework/languages/node/packages/framework/dist/runtime/backend/node/node-raw-binding-port.js')
assert hashlib.sha256((R/dist).read_bytes()).hexdigest()==manifest['candidate_js_sha256'],'runtime output changed outside this prototype'
items=json.loads((B/'sha256.json').read_text());result=[]
for rel,expected in items.items():
 src=B/rel;dst=R/rel;assert hashlib.sha256(src.read_bytes()).hexdigest()==expected,(rel,'bad baseline snapshot');shutil.copyfile(src,dst);assert hashlib.sha256(dst.read_bytes()).hexdigest()==expected;result.append({'file':rel,'sha256':expected})
(J/'runtime-restored.json').write_text(json.dumps({'reason':'reference-copy candidate rejected after measurement','restored':result},indent=2)+'\n');print('restored original runtime and build cache',len(result))
