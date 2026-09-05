2026-09-05T19:41:18+09:00 start detached preserved; load=2.71; inspect/profile then binding-only optimization
2026-09-05T19:43:15+09:00 profile-before DD+DR64 start;  19:43:15 up 1 day,  1:59,  1 user,  load average: 0.46, 4.29, 23.23
2026-09-05T19:43:47+09:00 profile-before start; pinned identical package runtime;  19:43:47 up 1 day,  2:00,  1 user,  load average: 0.28, 3.88, 22.49
2026-09-05T19:45:42+09:00 profile-before complete DD 41109 sends 11 ctypes/send; DR 27600 requests ~18 ctypes/request; load=0.31
2026-09-05T19:46:04+09:00 py-spy-before GIL start;  19:46:04 up 1 day,  2:02,  1 user,  load average: 0.20, 2.57, 19.39
20:33 RESUME after codex quota reset
2026-09-05T20:35:34.176986+09:00 resume: Python diff empty; before cProfile/GIL artifacts retained; inspecting eager SEND entry and native message boundary; load=1.3154296875
2026-09-05T20:38:57.682106+09:00 profile-after DD/DR64; load=2.44921875; related 57 PASS; Core hash matches before
2026-09-05T20:40:49.539311+09:00 python-pass1-after: start check load=2.04638671875; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:41:17.543117+09:00 python-pass1-after: running load=2.81396484375
2026-09-05T20:41:27.544396+09:00 python-pass1-after: INVALIDATED load=3.00341796875>3; stopping owned measurement group
2026-09-05T20:42:24.088793+09:00 python-pass1-storage-baseline: start check load=4.48193359375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-compare.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:43:54.493162+09:00 gate PASS 206 tests +4 subtests, samples 7/7; repeated related suite complete; waiting for quiet paired measurement load=2.88720703125
2026-09-05T20:45:50.796925+09:00 python-pass1-matched-before: not started: other measurement=['3646386:bash', '3649954:bash', '3650042:perf_multi', '3650064:perf_multi']; load=2.66015625
2026-09-05T20:48:52.373921+09:00 SEND registry now token-only, shared owner condition; related 71 PASS; other Go measurement still active, load=2.828125
2026-09-05T20:49:22.328170+09:00 python-pass1-matched-before: not started: other measurement=['3649954:bash']; load=2.5322265625
2026-09-05T20:51:56.557107+09:00 python-pass1-matched-before: waiting: load=2.41015625, other=[]
2026-09-05T20:52:26.565684+09:00 python-pass1-matched-before: waiting: load=1.9072265625, other=['3663877:bash', '3663924:perf_multi_deal', '3663936:bash', '3663938:perf_multi_deal']
2026-09-05T20:52:56.571866+09:00 python-pass1-matched-before: waiting: load=2.33984375, other=[]
2026-09-05T20:53:26.578715+09:00 python-pass1-matched-before: waiting: load=2.13134765625, other=[]
2026-09-05T20:53:56.585528+09:00 python-pass1-matched-before: waiting: load=2.66015625, other=[]
2026-09-05T20:54:26.593034+09:00 python-pass1-matched-before: waiting: load=2.5205078125, other=[]
2026-09-05T20:54:56.600137+09:00 python-pass1-matched-before: waiting: load=2.22265625, other=[]
2026-09-05T20:55:19.388211+09:00 python-pass1-matched-before: start check load=2.04150390625; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-compare.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:55:21.401692+09:00 python-pass1-matched-before: INVALIDATED load=2.04150390625, other=['3667850:python', '3667872:python']; stopping owned measurement group
2026-09-05T20:56:52.167819+09:00 python-pass1-matched-before: start check load=1.1474609375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-compare.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:57:06.215764+09:00 python-pass1-matched-before: complete rc=0; max_load=1.484375
2026-09-05T20:57:06.258407+09:00 python-pass1-matched-after: start check load=1.484375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-compare.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:57:22.314581+09:00 python-pass1-matched-after: complete rc=0; max_load=1.81494140625
2026-09-05T20:57:43.834709+09:00 python-pass1-after-final: start check load=1.5986328125; command=['bash', 'bindings/python/perf/run_benchmarks_multi.sh', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB', '--transports', 'tcp', '--msg-sizes', '64,256,1024,4096,65536', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T20:58:11.936956+09:00 python-pass1-after-final: running load=1.74267578125
2026-09-05T20:58:42.036021+09:00 python-pass1-after-final: running load=1.7275390625
2026-09-05T20:59:12.134671+09:00 python-pass1-after-final: running load=1.75146484375
2026-09-05T20:59:42.252995+09:00 python-pass1-after-final: complete rc=0; max_load=2.3017578125
2026-09-05T21:01:14.408606+09:00 python-pass1-cprofile-before: start check load=0.46923828125; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:01:28.453943+09:00 python-pass1-cprofile-before: complete rc=0; max_load=0.59228515625
2026-09-05T21:01:28.489515+09:00 python-pass1-cprofile-after: start check load=0.6611328125; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:01:42.534985+09:00 python-pass1-cprofile-after: complete rc=0; max_load=0.86083984375
2026-09-05T21:01:42.572062+09:00 python-pass1-memray-before: start check load=0.86083984375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:01:58.624493+09:00 python-pass1-memray-before: complete rc=0; max_load=0.9658203125
2026-09-05T21:01:58.667096+09:00 python-pass1-memray-after: start check load=0.88818359375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:02:11.333819+09:00 final official after 20/20 complete, RESULT100/100 fail0 maxload2.302; historical throughput mean Py/C DD9.51 DR15.59 RR17.53 PUB28.51 percent; final gate207+4/samples7; profile census running; load=1.052734375
2026-09-05T21:02:14.715861+09:00 python-pass1-memray-after: complete rc=0; max_load=1.208984375
2026-09-05T21:02:14.754796+09:00 python-pass1-gil-before: start check load=1.208984375; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/py-spy', 'record', '--gil', '--subprocesses', '--rate', '200', '--format', 'speedscope', '-o', '/home/hep7hep7/project/zlink-work/c016/reports/python-pass1-final-gil-before/gil.json', '--', '/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:02:28.800409+09:00 python-pass1-gil-before: complete rc=1; max_load=1.3525390625
2026-09-05T21:03:46.431517+09:00 python-pass1-gil-after: start check load=0.4453125; command=['/home/hep7hep7/project/zlink-work/c016/python-venv/bin/py-spy', 'record', '--gil', '--subprocesses', '--rate', '200', '--format', 'speedscope', '-o', '/home/hep7hep7/project/zlink-work/c016/reports/python-pass1-final-gil-after/gil.json', '--', '/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python', '/home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py', 'parent', '--pattern', 'DEALER_DEALER,DEALER_ROUTER_REQREP', '--transports', 'tcp', '--msg-sizes', '64', '--clients', '100', '--duration', '5', '--runs', '1']
2026-09-05T21:04:00.473487+09:00 python-pass1-gil-after: complete rc=0; max_load=0.8828125
2026-09-05T21:10:07.420528+09:00 census complete DD ctypes11.04->4.02, Python111->78.69, binding allocator events307->188/op; deterministic early-WRITABLE race PASS; final test-only gate running; load=0.4423828125
2026-09-05T21:17:26.839224+09:00 final gate208+4 PASS samples7/7; related72x5 PASS; API signatures unchanged, diff-check PASS; writing summary and evidence manifest; load=0.23681640625
2026-09-05T21:19:17.415833+09:00 COMPLETE pass1 source4+test1; final after copied; summary written; gate208+4, samples7/7, related72x5 PASS; throughput target and full zero-allocation unmet; load=0.0732421875
EXIT:2
