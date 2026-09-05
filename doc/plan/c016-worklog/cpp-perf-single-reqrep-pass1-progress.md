2026-09-05T20:43:44+09:00 START: inspect single REQREP; load gate <=3, scope bindings/cpp only
2026-09-05T20:45:46.285094+09:00 native c: load1=2.28
2026-09-05T20:45:48.357900+09:00 native c EXIT=0
2026-09-05T20:45:48.358030+09:00 native cpp: load1=2.66
2026-09-05T20:46:13.831731+09:00 native cpp: load1=2.96
2026-09-05T20:46:14.949693+09:00 native cpp EXIT=0
2026-09-05T20:46:14.949764+09:00 callgrind c: load1=2.96
2026-09-05T20:46:16.872950+09:00 callgrind c EXIT=1
2026-09-05T20:46:16.873085+09:00 callgrind cpp: load1=2.80
2026-09-05T20:46:18.544700+09:00 callgrind cpp EXIT=0
2026-09-05T20:47:35.926121+09:00 timeline c: load1=2.96
2026-09-05T20:47:38.001810+09:00 timeline c EXIT=0
2026-09-05T20:47:38.001968+09:00 timeline cpp: load1=2.80
2026-09-05T20:47:39.088240+09:00 timeline cpp EXIT=0
2026-09-05T20:48:22.628335+09:00 callgrind c diagnose setup failure: load1=2.56
2026-09-05T20:48:26.474200+09:00 callgrind c debug EXIT=1
2026-09-05T20:49:19.184407+09:00 callgrind c fair scheduler (default starvation): load1=2.75
2026-09-05T20:49:21.778773+09:00 callgrind c debug EXIT=0
2026-09-05T20:49:58.355364+09:00 timeline c: load1=2.39
2026-09-05T20:50:00.450402+09:00 timeline c EXIT=0
2026-09-05T20:50:00.450485+09:00 callgrind cpp fair match C: load1=2.39
2026-09-05T20:50:02.059304+09:00 callgrind cpp fair EXIT=0
2026-09-05T20:52:06.802537+09:00 native isolated c: load1=2.04 other_bench=['3663482:bash', '3663529:perf_multi_deal', '3663541:bash', '3663543:perf_multi_deal']
2026-09-05T20:52:26.810745+09:00 native isolated c: load1=1.91 other_bench=['3663877:bash', '3663924:perf_multi_deal', '3663936:bash', '3663938:perf_multi_deal']
2026-09-05T20:52:46.819471+09:00 native isolated c: load1=2.22 other_bench=[]
2026-09-05T20:52:48.899399+09:00 native isolated c EXIT=0
2026-09-05T20:52:48.906595+09:00 native isolated cpp: load1=2.28 other_bench=[]
2026-09-05T20:52:49.972756+09:00 native isolated cpp EXIT=0
2026-09-05T20:52:49.980095+09:00 timeline isolated cpp: load1=2.28 other_bench=['3664385:bash', '3664432:perf_multi_deal', '3664444:bash', '3664446:perf_multi_deal']
2026-09-05T20:53:09.988702+09:00 timeline isolated cpp: load1=1.82 other_bench=[]
2026-09-05T20:53:11.069493+09:00 timeline isolated cpp EXIT=0
2026-09-05T20:53:11.076011+09:00 after official no-go unchanged source: load1=1.82 other_bench=['3665331:bash', '3665378:perf_multi_deal', '3665390:bash', '3665392:perf_multi_deal']
2026-09-05T20:53:31.084066+09:00 after official no-go unchanged source: load1=1.96 other_bench=[]
2026-09-05T20:54:16.085325+09:00 after RUNNING load1=2.13
2026-09-05T20:54:56.875350+09:00 INVALIDATE partial after: external Rust benchmark started during our run; stop owned measurement, restart only after 60s quiet + load<=3
2026-09-05T20:55:27.245756+09:00 after official no-go unchanged source: load1=2.04 other_bench=['3668015:bash', '3668062:perf_multi_pubs', '3668077:perf_multi_pubs']
2026-09-05T20:55:47.251946+09:00 after official no-go unchanged source: load1=2.23 other_bench=[]
2026-09-05T20:56:07.257684+09:00 after official no-go unchanged source: load1=2.04 other_bench=[]
2026-09-05T20:56:27.264014+09:00 after official no-go unchanged source: load1=1.54 other_bench=[]
2026-09-05T20:56:47.271045+09:00 after official no-go unchanged source: load1=1.16 other_bench=[]
2026-09-05T20:57:32.272209+09:00 after RUNNING load1=1.77
2026-09-05T20:57:45.154596+09:00 INVALIDATE after: overlap detected own=['3671988:cpp_perf_dealer'] other=['3672092:python3']; terminate owned measurement only
2026-09-05T21:01:03.541147+09:00 final release diagnostics WAIT load1=0.55 other=[]
2026-09-05T21:01:33.635557+09:00 final release diagnostics WAIT load1=0.85 other=['3676517:python:/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python /home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py child /home/hep7hep7/project/zlink-wt-python-perf/bindi', '3676523:python:/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python /home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py child /home/hep7hep7/project/zlink-wt-python-perf/bindi']
2026-09-05T21:02:03.728344+09:00 final release diagnostics WAIT load1=1.06 other=['3677063:python:/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python /home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py child /home/hep7hep7/project/zlink-wt-python-perf/bindi', '3677070:python:/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python /home/hep7hep7/project/zlink-work/c016/python-pass1-capture.py child /home/hep7hep7/project/zlink-wt-python-perf/bindi']
2026-09-05T21:02:33.825876+09:00 final release diagnostics WAIT load1=1.13 other=[]
2026-09-05T21:03:03.931407+09:00 final release diagnostics WAIT load1=0.74 other=[]
2026-09-05T21:03:30.024592+09:00 final release diagnostics START load1=0.49 quiet=60s
2026-09-05T21:03:30.028922+09:00 native release c WAIT load1=0.49 other=[]
2026-09-05T21:03:30.028958+09:00 native release c START load1=0.49 quiet=0s
2026-09-05T21:03:32.557915+09:00 c-native-release EXIT=0 elapsed=2.5s max_load=0.49
2026-09-05T21:03:32.565510+09:00 native release cpp WAIT load1=0.49 other=[]
2026-09-05T21:03:32.565547+09:00 native release cpp START load1=0.49 quiet=0s
2026-09-05T21:03:34.083618+09:00 cpp-native-release EXIT=0 elapsed=1.5s max_load=0.53
2026-09-05T21:03:34.089956+09:00 callgrind release c WAIT load1=0.53 other=[]
2026-09-05T21:03:34.090002+09:00 callgrind release c START load1=0.53 quiet=0s
2026-09-05T21:03:37.123954+09:00 c-callgrind-release EXIT=0 elapsed=3.0s max_load=0.53
2026-09-05T21:03:37.128886+09:00 callgrind release cpp WAIT load1=0.53 other=[]
2026-09-05T21:03:37.128925+09:00 callgrind release cpp START load1=0.53 quiet=0s
2026-09-05T21:03:39.150630+09:00 cpp-callgrind-release EXIT=0 elapsed=2.0s max_load=0.53
2026-09-05T21:03:39.156596+09:00 timeline release cpp WAIT load1=0.48 other=[]
2026-09-05T21:03:39.156635+09:00 timeline release cpp START load1=0.48 quiet=0s
2026-09-05T21:03:40.677366+09:00 cpp-timeline-release EXIT=0 elapsed=1.5s max_load=0.48
2026-09-05T21:03:40.683073+09:00 after complete WAIT load1=0.48 other=[]
2026-09-05T21:04:10.783498+09:00 after complete WAIT load1=0.75 other=[]
2026-09-05T21:04:40.886480+09:00 after complete WAIT load1=0.45 other=[]
2026-09-05T21:05:00.954614+09:00 after complete START load1=0.32 quiet=60s
2026-09-05T21:05:46.009921+09:00 after-complete RUNNING load1=0.89
2026-09-05T21:06:31.097273+09:00 after-complete RUNNING load1=1.28
2026-09-05T21:07:16.134478+09:00 after-complete RUNNING load1=1.21
2026-09-05T21:08:01.146806+09:00 after-complete RUNNING load1=0.93
2026-09-05T21:08:16.315616+09:00 after-complete EXIT=0 elapsed=195.4s max_load=1.44
2026-09-05T21:08:16.818280+09:00 gate-configure EXIT=0 elapsed=0.5s max_load=0.80
2026-09-05T21:08:44.832949+09:00 gate EXIT=0 elapsed=28.0s max_load=1.53
2026-09-05T21:08:45.334447+09:00 repeat-configure EXIT=0 elapsed=0.5s max_load=1.53
2026-09-05T21:08:46.836815+09:00 contract-repeat EXIT=0 elapsed=1.5s max_load=1.53
2026-09-05T21:08:47.338195+09:00 diff-check EXIT=0 elapsed=0.5s max_load=1.53
2026-09-05T21:08:47.338259+09:00 FINAL GATE official=0 repeat=0 diff=0
2026-09-05T21:11:07.241424+09:00 COMPLETE no-go; tracked diff=0; after36/36 RESULT180/180 fail0 maxload1.44 overlap0; contract16/16 sample7/7 related5x5 pass; Core SHA unchanged; summary finalized
EXIT:0
