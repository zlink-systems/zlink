2026-09-05T22:28:58+09:00 START: inspection
2026-09-05T22:30:37+09:00 inspection: baseline built; load 0.34
2026-09-05T22:31:55.636776+09:00 python-pass2-cprofile-before: start check load=0.52587890625; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T22:31:57.643782+09:00 python-pass2-cprofile-before: complete rc=1; max_load=0.52587890625
2026-09-05T22:33:05+09:00 design: stable per-part storage; no pools or new wake sources; profile launch investigation
2026-09-05T22:33:17.864856+09:00 python-pass2-cprofile-before: start check load=0.58837890625; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T22:33:19.871281+09:00 python-pass2-cprofile-before: complete rc=1; max_load=0.58837890625
2026-09-05T22:36:07+09:00 implementation: C stable storage materialize/submit and receive wrapper construction; targeted tests completed
2026-09-05T22:42:00+09:00 implementation: native entry init/clone/send completion; exact Core copy sha256=d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00
2026-09-05T22:42:15.012041+09:00 python-pass2-cprofile-intermediate: start check load=1.02294921875; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T22:42:25.045979+09:00 python-pass2-cprofile-intermediate: complete rc=0; max_load=1.1796875
2026-09-05T22:46:17+09:00 intermediate profile DD binding=17.20/message (pass1=35.19); runner=6.07/message; receive fused; completion state migration
2026-09-05T22:51:26+09:00 receive+entry gates: 52 passed; native Future delivery implementation; load 1.17
2026-09-05T22:54:33+09:00 completion drain loop migrated to C with existing FFI fault-injection seam; no queue/registry policy change
2026-09-05T23:02:45+09:00 send path fused: materialization, per-part submit and token registration in start_send; prior concurrency failure fixed with per-part GIL boundaries
2026-09-05T23:06:23+09:00 targeted gate: 58 passed; full tests and samples started
2026-09-05T23:09:48+09:00 full gate before final request fusion: 214 tests + 4 subtests, samples 7/7; start_request now follows same C submission boundary as SEND
2026-09-05T23:10:20.540024+09:00 python-pass2-cprofile-after: start check load=0.35546875; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T23:10:30.569227+09:00 python-pass2-cprofile-after: complete rc=0; max_load=0.4072265625
2026-09-05T23:11:13+09:00 final profiling in progress; load 0.23
2026-09-05T23:11:42.245402+09:00 python-pass2-after: start check load=0.13916015625; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T23:12:10.330329+09:00 python-pass2-after: running load=0.7529296875
2026-09-05T23:12:40.413927+09:00 python-pass2-after: running load=1.29345703125
2026-09-05T23:13:10.499034+09:00 python-pass2-after: running load=1.4287109375
2026-09-05T23:13:40.594754+09:00 python-pass2-after: complete rc=0; max_load=1.857421875
2026-09-05T23:19:53.270050+09:00 python-pass2-after-final: start check load=0.2578125; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T23:19:54+09:00 interned-name build: no compiler warnings; 58 targeted tests passed; final after started load=0.40
2026-09-05T23:20:21.356908+09:00 python-pass2-after-final: running load=0.962890625
2026-09-05T23:20:51.442844+09:00 python-pass2-after-final: running load=1.4638671875
2026-09-05T23:21:21.529285+09:00 python-pass2-after-final: running load=1.9697265625
2026-09-05T23:21:51.628530+09:00 python-pass2-after-final: complete rc=0; max_load=2.783203125
2026-09-05T23:23:31.867208+09:00 python-pass2-cprofile-final: start check load=1.01025390625; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T23:23:33+09:00 after-final completed: status complete, 20/20 cells; DD 33.7-37.7k msg/s; reports copied; final profiling + 5x gate next
2026-09-05T23:23:41.895467+09:00 python-pass2-cprofile-final: complete rc=0; max_load=1.01025390625
2026-09-05T23:28:52.043625+09:00 python-pass2-after-complete: start check load=0.171875; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T23:29:11+09:00 Message.data + close native boundary added; writable ctypes view/cache preserved; 59 targeted passed; final measurement running
2026-09-05T23:29:20.127849+09:00 python-pass2-after-complete: running load=0.7958984375
2026-09-05T23:29:50.212034+09:00 python-pass2-after-complete: running load=0.79638671875
2026-09-05T23:30:20.297246+09:00 python-pass2-after-complete: running load=1.44287109375
2026-09-05T23:30:50.398964+09:00 python-pass2-after-complete: complete rc=0; max_load=2.783203125
2026-09-05T23:31:25.338646+09:00 python-pass2-cprofile-complete: start check load=1.56201171875; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T23:31:35.369190+09:00 python-pass2-cprofile-complete: complete rc=0; max_load=1.56201171875
2026-09-05T23:35:43+09:00 completion capture/join now C; existing error/cancellation seam retained; final targeted including request-WRITABLE tests running
2026-09-05T23:37:43+09:00 final related gate 5/5: 79 passed each; full gate running
2026-09-05T23:37:59.580641+09:00 python-pass2-after-verified: start check load=0.2060546875; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T23:38:27.666281+09:00 python-pass2-after-verified: running load=0.8408203125
2026-09-05T23:38:47+09:00 final full gate: 215 passed + 4 subtests; samples 7/7; diff check and public contracts/runner diff clean
2026-09-05T23:38:57.748682+09:00 python-pass2-after-verified: running load=1.3271484375
2026-09-05T23:39:27.834826+09:00 python-pass2-after-verified: running load=1.5537109375
2026-09-05T23:39:57.936482+09:00 python-pass2-after-verified: complete rc=0; max_load=1.95751953125
2026-09-05T23:42:02+09:00 final cleanup contract fixed (Exception vs BaseException); 80 related tests passed in each of 5 runs; final full gate started
2026-09-05T23:43:09.856889+09:00 python-pass2-after-release: start check load=0.125; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T23:43:11+09:00 final full gate: 216 passed + 4 subtests; samples 7/7; final release measurement running
2026-09-05T23:43:37.942821+09:00 python-pass2-after-release: running load=0.96240234375
2026-09-05T23:44:08.027322+09:00 python-pass2-after-release: running load=1.2060546875
2026-09-05T23:44:38.108977+09:00 python-pass2-after-release: running load=1.18212890625
2026-09-05T23:45:08.210192+09:00 python-pass2-after-release: complete rc=0; max_load=1.67041015625
2026-09-05T23:45:27+09:00 final checks remain: finish after, refresh cProfile census, write summary and EXIT; load=1.20
2026-09-05T23:45:46.529027+09:00 python-pass2-cprofile-release: start check load=0.85498046875; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass2-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '3', '--runs', '1']
2026-09-05T23:45:56.559222+09:00 python-pass2-cprofile-release: complete rc=0; max_load=0.85498046875
2026-09-05T23:50:26+09:00 final census complete; artifact audit: benchmark/profile load gate passed; progress 3-minute interval missed in 10 earlier gaps (max 492s), no backdated entries
