from pathlib import Path
import subprocess, sys
root=Path('/root/worktree/zlink-3867e/framework/languages/cpp')
job=Path('/root/jobs/fx/cx/v21/s033/cpr2')
wrapper='/root/jobs/fx/cx/measure-shared.sh'
commands=[
 ('coordination-cases.log',[str(root/'build/linux-ninja-release/test_cpp_framework_request_call_allocations'),'--gtest_filter=RequestCallAllocations.UnknownNode*DeliversOnce','--gtest_color=no']),
 ('ctest-call-fixed.log',[wrapper,'ctest','--preset','linux-ninja-release','-R','^test_cpp_framework_request_call_allocations$','--output-on-failure','-V']),
 ('ctest-final.log',[wrapper,'ctest','--preset','linux-ninja-release','-L','^(unit|contract)$','--output-on-failure']),
]
for name,command in commands:
 print('START '+name,flush=True)
 with (job/name).open('w') as output:
  result=subprocess.run(command,cwd=root,stdout=output,stderr=subprocess.STDOUT)
 print(f'END {name} exit={result.returncode}',flush=True)
 if result.returncode:
  sys.exit(result.returncode)
 if name=='coordination-cases.log':
  assert '[==========] Running 4 tests' in (job/name).read_text(), 'Expected four coordination cases'
