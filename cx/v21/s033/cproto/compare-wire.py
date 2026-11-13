import pathlib,sys
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto'); main=j/'wire-main'; after=j/('wire-'+sys.argv[1]); files=sorted(p.name for p in main.glob('*.bin'))
assert len(files)==12,files
def compare(name,a,b):
 if a!=b: raise ValueError(f'wire differs: {name}, main={len(a)}, after={len(b)}')
for name in files:
 a=(main/name).read_bytes();b=(after/name).read_bytes()
 try: compare(name,a,b)
 except ValueError as e: raise SystemExit(str(e))
print('wire byte identity: PASS, 12/12 frames, 6 request cases')
a=(main/'0.bin').read_bytes(); b=bytearray((after/'0.bin').read_bytes());b[len(b)//2]^=1
try: compare('0.bin intentional mutation',a,bytes(b))
except ValueError as e: print('intentional one-byte mutation: detected:',e)
else: raise SystemExit('intentional one-byte mutation was not rejected')
