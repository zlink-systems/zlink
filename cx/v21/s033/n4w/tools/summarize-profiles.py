import sys,json,pathlib,collections
job=pathlib.Path("/root/jobs/fx/cx/v21/s033/n4w")
tag=sys.argv[1]
summary=[]
for out in sorted((job/"raw").glob(tag+"-L*-*")):
  for cpu in out.glob("*.cpuprofile"):
    data=json.loads(cpu.read_text()); pid=cpu.name.split(".")[3]
    # The source script has main/createRawTransport; the target has schedulePump.
    role=next((r for r,p in json.loads((out/"lifetime.json").read_text())["pids"].items() if str(p)==pid), "unknown")
    nodes={n["id"]:n for n in data["nodes"]}; parents={}
    for node in data["nodes"]:
      for child in node.get("children",[]): parents[child]=node["id"]
    anchor=json.loads((out/f"clock-{pid}.json").read_text()); window=json.loads((out/"active-window.json").read_text())
    active_lo=anchor["hrUs"]+(window["startUnixMs"]-anchor["unixMs"])*1000
    active_hi=anchor["hrUs"]+(window["endUnixMs"]-anchor["unixMs"])*1000
    assert abs(data["startTime"]-anchor["hrUs"])<10_000_000, "CPU profile clock mismatch"
    timestamp=data["startTime"]
    own=collections.Counter(); groups=collections.Counter(); groups_samples=collections.Counter()
    for n,dt in zip(data.get("samples",[]),data.get("timeDeltas",[])):
      timestamp+=dt
      if not active_lo <= timestamp <= active_hi: continue
      cf=nodes[n]["callFrame"]; key=f'{cf["functionName"]} {cf["url"]}:{cf["lineNumber"]+1}'
      own[key]+=1
      stack=[]; index=n
      while index in nodes:
        stack.append(nodes[index]["callFrame"]["functionName"])
        if index not in parents: break
        index=parents[index]
      for group,names in {"base64_encode_union":{"encodeBase64","encodeNodeBase64","btoa","base64Slice"},"base64_decode_union":{"decodeBase64","decodeNodeBase64","atob","base64Write"}}.items():
        if any(x in stack for x in names): groups[group]+=1;groups_samples[group]+=1
      for name in ("encodeBase64","btoa","base64Slice","base64Write","decodeBase64","decodeNodeBase64","atob","prepareNodeJsonBytes","validateFrameworkJsonV1Value","toJSON","frameworkJsonV1Replacer","stringifyFrameworkJsonV1", "nodeJsonReplacer", "encodeNodeBase64", "parseFrameworkJsonV1","scan", "decodeChannelPayload","encodePayload","(garbage collector)"):
        if name in stack: groups[name]+=1;groups_samples[name]+=1
    allocfile=out/f"alloc-{pid}.json"; total_alloc=0; allocown=collections.Counter(); allocgroups=collections.Counter()
    if allocfile.exists():
      alloc=json.loads(allocfile.read_text())
      def walk(node,stack):
        global total_alloc
        cf=node["callFrame"]; size=node.get("selfSize",0);total_alloc+=size
        name=cf["functionName"];stack=stack+[name]
        allocown[f'{name} {cf["url"]}:{cf["lineNumber"]+1}']+=size
        for group in ("encodeBase64","btoa","base64Slice","base64Write","decodeBase64","decodeNodeBase64","atob","prepareNodeJsonBytes","validateFrameworkJsonV1Value","toJSON","frameworkJsonV1Replacer","stringifyFrameworkJsonV1", "nodeJsonReplacer", "encodeNodeBase64", "parseFrameworkJsonV1", "decodeChannelPayload", "encodePayload"):
          if group in stack: allocgroups[group]+=size
        for child in node.get("children",[]):walk(child,stack)
      if "head" in alloc: walk(alloc["head"],[])
    total=sum(own.values())
    result=dict(cell=out.name,role=role,pid=pid,total_samples=total,cpu_subtree_pct={k:round(100*v/total,2) for k,v in groups.items()},cpu_subtree_samples=dict(groups_samples),top_cpu=own.most_common(18),sampled_allocation_bytes=total_alloc,allocation_subtree_bytes=dict(allocgroups),top_allocation=allocown.most_common(18))
    (out/f"summary-{pid}.json").write_text(json.dumps(result,indent=2));summary.append(result)
(job/"raw"/f"{tag}-summary.json").write_text(json.dumps(summary,indent=2))
for r in summary:
 print(r["cell"],r["role"],r["pid"],r["cpu_subtree_pct"],"alloc",r["allocation_subtree_bytes"])
 print("CPU",[(x.split("file:///")[-1],v) for x,v in r["top_cpu"][:7]])
