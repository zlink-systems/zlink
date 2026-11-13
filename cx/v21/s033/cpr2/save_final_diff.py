from pathlib import Path
import subprocess
root=Path('/root/worktree/zlink-3867e')
job=Path('/root/jobs/fx/cx/v21/s033/cpr2')
patch=subprocess.check_output(['git','diff','--binary'],cwd=root)
for path in [
 'framework/languages/cpp/framework/src/runtime/protocol/application_envelope_encoder.hpp',
 'framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_wire_identity.cpp',
 'framework/languages/cpp/tests/fixtures/request-wire-main-213e579f52.json',
]:
 result=subprocess.run(['git','diff','--no-index','--binary','--','/dev/null',path],cwd=root,capture_output=True)
 if result.returncode!=1:
  raise RuntimeError(f'Unexpected diff exit for {path}: {result.returncode}')
 patch+=result.stdout
(job/'final.diff').write_bytes(patch)
subprocess.run(['git','apply','--check','--reverse',str(job/'final.diff')],cwd=root,check=True)
print(f'final.diff: {len(patch)} bytes; reverse application check passed')
