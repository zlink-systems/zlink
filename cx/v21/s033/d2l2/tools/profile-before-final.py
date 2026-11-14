import pathlib,os,time,subprocess
inode=os.stat("/root/jobs/fx/cx/measure.lock").st_ino
def hold():
 return any("FLOCK" in line and "WRITE" in line and "->" not in line and any(x.endswith(":"+str(inode)) for x in line.split()) for line in pathlib.Path("/proc/locks").read_text().splitlines())
for pattern,size,cell in (("request-serial",4096,"serial4K"),("request-backpressure",1024,"bp1K")):
 for level in ("L1","L2"):
  for stage in ("warm","profile"):
   while hold():time.sleep(2)
   label=f"before-{level}-{cell}-v4"+("-warm" if stage=="warm" else "")
   print(label,flush=True)
   subprocess.run(["python3","/root/jobs/fx/cx/v21/s033/d2l2/tools/profile.py",label,level,pattern,str(size)],check=True)
