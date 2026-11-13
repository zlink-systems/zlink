import os, sys, json, time, signal, subprocess, urllib.request, pathlib, hashlib
job = pathlib.Path("/root/jobs/fx/cx/v21/s033/n4json")
root = pathlib.Path("/root/worktree/zlink-v21-s033/n4json-scratch")
bench = root / "framework/bench/grpc/node"
tag = sys.argv[1] if len(sys.argv) > 1 else "before"

def http(port, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                                     headers={"Content-Type":"application/json"})
    with urllib.request.urlopen(request, timeout=.7) as response:
        return json.load(response)

def snapshot(procs, label, output):
    with (output / "scheduling.txt").open("a") as f:
        f.write(label + "\n")
        for role, p in procs:
            for task in sorted(pathlib.Path(f"/proc/{p.pid}/task").iterdir()):
                status=(task / "status").read_text()
                f.write(f"{role} pid={p.pid} tid={task.name} " + next(x for x in status.splitlines() if x.startswith("Cpus_allowed_list:")) + "\n")
                f.write(subprocess.check_output(["chrt", "-p", task.name], text=True))
                assert os.sched_getaffinity(int(task.name)) <= {6,7,14,15}, "profile thread escaped housekeeping CPUs"

for step in ("L3", "L4"):
  for pattern, size in (("send-saturation",4096),("request-serial",1024)):
    output = job / "raw" / f"{tag}-{step}-{pattern}-{size}"
    output.mkdir(exist_ok=False)
    env=os.environ.copy()
    for key in list(env):
      if key.startswith("ZLINK_LADDER_"): del env[key]
    env.update(NODE_PATH=str(root/"framework/languages/node/node_modules"),PROFILE_DIR=str(output),
      ZLINK_LADDER_L0_CLIENT="1",ZLINK_LADDER_L0_SERVER="1",ZLINK_LADDER_L1_PERMIT="1",
      ZLINK_LADDER_L2_ENVELOPE="1",ZLINK_LADDER_L3_SERVICE_WIRE="1",
      ZLINK_LADDER_L2_CLIENT="1", ZLINK_LADDER_L3_CLIENT="1")
    if step == "L4": env.update(ZLINK_LADDER_L4_CLIENT="1",ZLINK_LADDER_L4_SERVER="1")
    node=["taskset","-c","6-7,14-15","node","--cpu-prof",f"--cpu-prof-dir={output}",
          f"--require={job}/tools/allocation-preload.cjs"]
    target_args=["zlink-framework-server/main.js","--endpoint","tcp://127.0.0.1:26334",
                 "--command-endpoint","tcp://127.0.0.1:26336","--metrics-url","http://127.0.0.1:26335"]
    source_args=["client/main.js","--implementation","zlink-framework-node","--scenario",pattern,
      "--payload-size",str(size),"--warmup-seconds","0","--trigger-url","http://127.0.0.1:26330",
      "--stats-url","http://127.0.0.1:26331","--target-endpoint","tcp://127.0.0.1:26334",
      "--target-command-endpoint","tcp://127.0.0.1:26336","--target-stats-url","http://127.0.0.1:26335",
      "--output",str(output)]
    primary=pathlib.Path("/root/worktree/zlink-v21-s033/n4json")
    def git(*args): return subprocess.check_output(["git", *args],cwd=primary,text=True).strip()
    provenance={"scratch_archive":"ae8a5178b5","primary_head":git("rev-parse","HEAD"),
      "primary_dirty":bool(git("status","--porcelain")),"primary_base":git("merge-base","HEAD","origin/main"),
      "archive_base":git("merge-base","ae8a5178b5","origin/main"),
      "preload_sha256":hashlib.sha256((job/"tools/allocation-preload.cjs").read_bytes()).hexdigest()}
    (output/"provenance.json").write_text(json.dumps(provenance,indent=2))
    with (job/"progress.md").open("a") as f:
      f.write("\n"+json.dumps(provenance)+"\n")
      f.write(f"\nProfile {output.name}: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.\n")
      for rel in ("framework/bench/grpc/node/client/main.js", "framework/bench/grpc/node/shared/raw-wire.js",
                  "framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js",
                  "framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js",
                  "framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node"):
        file=root/rel
        f.write(f"{file}: sha256={hashlib.sha256(file.read_bytes()).hexdigest()}\n")
      src=root/"framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts"
      assert (root/"framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js").stat().st_mtime >= src.stat().st_mtime
      f.write("Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.\n")
    procs=[]; handles=[]; failure=None; started=time.monotonic()
    try:
      for role,args in (("target",target_args),("source",source_args)):
        log=(output/f"{role}.log").open("w"); handles.append(log)
        p=subprocess.Popen(node+args,env=env,cwd=bench,stdout=log,stderr=subprocess.STDOUT)
        procs.append((role,p))
      (output/"commands.json").write_text(json.dumps({"target":node+target_args,"source":node+source_args,"env":{k:v for k,v in env.items() if k.startswith("ZLINK_LADDER_")}},indent=2))
      while True:
        if time.monotonic()-started > 7: raise RuntimeError("ready deadline within 20s profiling cap")
        if any(p.poll() is not None for _,p in procs): raise RuntimeError("process exited before ready")
        try:
          if http(26331,"/bench/stats").get("ready"): break
        except (OSError,ValueError): pass
        time.sleep(.05)
      snapshot(procs,"start",output)
      for phase,duration in (("warmup",3000),("active",4000)):
        if phase=="active":
          (output/"warmup-target-stats.json").write_text(json.dumps(http(26335,"/bench/stats"),indent=2))
          (output/"target-reset.json").write_text(json.dumps(http(26335,"/bench/reset",{})))
        trigger=dict(runId=output.name,cellId=f"{pattern}-{size}",pattern=pattern,payloadBytes=size,
                     phase=phase,durationMs=duration,requestWindow=100,submitConcurrency=8)
        window_start=time.time()*1000
        (output/f"{phase}-trigger.json").write_text(json.dumps(http(26330,"/bench/start",trigger)))
        phase_start=time.monotonic(); mid=False
        while True:
          if time.monotonic()-started > 16: raise RuntimeError("stop before 20s profiling cap")
          stats=http(26331,"/bench/stats")
          if phase=="active" and not mid and time.monotonic()-phase_start>2:
            snapshot(procs,"mid-active",output); mid=True
          if stats["phase"]=="failed": raise RuntimeError(str(stats))
          if stats["phase"]=="idle": break
          time.sleep(.1)
        (output/f"{phase}-window.json").write_text(json.dumps({"startUnixMs":window_start,"endUnixMs":time.time()*1000}))
        (output/f"{phase}-source-stats.json").write_text(json.dumps(stats,indent=2))
      (output/"target-stats.json").write_text(json.dumps(http(26335,"/bench/stats"),indent=2))
    except Exception as e:
      failure=e
      (output/"error.txt").write_text(repr(e))
      print(output.name,"FAILED",e,flush=True)
    finally:
      for _,p in reversed(procs):
        if p.poll() is None: p.terminate()
      for _,p in procs:
        try: p.wait(timeout=max(.1,19-(time.monotonic()-started)))
        except subprocess.TimeoutExpired: p.kill(); p.wait()
      for h in handles: h.close()
      (output/"lifetime.json").write_text(json.dumps({"wall_seconds":time.monotonic()-started,"exit_codes":{r:p.returncode for r,p in procs},"pids":{r:p.pid for r,p in procs}}))
    print(output.name,"done",flush=True)
    if failure is not None: sys.exit(1)
