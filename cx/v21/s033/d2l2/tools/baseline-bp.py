import pathlib,os,time,subprocess
inode=os.stat("/root/jobs/fx/cx/measure.lock").st_ino
def hold():
 return any("FLOCK" in line and "WRITE" in line and "->" not in line and any(x.endswith(":"+str(inode)) for x in line.split()) for line in pathlib.Path("/proc/locks").read_text().splitlines())
for level in ("L1","L2"):
 for stage in ("warm","profile"):
  deadline=time.monotonic()+1800
  while hold() and time.monotonic()<deadline:time.sleep(2)
  if hold():raise RuntimeError("other hold did not finish")
  label=f"before-{level}-bp1K-final-v2"+("-warm" if stage=="warm" else "")
  print(label,flush=True)
  subprocess.run(["python3","/root/jobs/fx/cx/v21/s033/d2l2/tools/profile.py",label,level,"request-backpressure","1024"],check=True)
