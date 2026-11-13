import pathlib,subprocess
p=pathlib.Path(__file__).parent
subprocess.run(['/root/jobs/fx/cx/measure-shared.sh','python3',str(p/'save-direct-build.py')],check=True)
subprocess.run(['/root/jobs/fx/cx/measure-shared.sh','taskset','-c','0-7','bash','java/build.sh'],cwd='/root/worktree/zlink-jsend/framework/bench/grpc',check=True)
