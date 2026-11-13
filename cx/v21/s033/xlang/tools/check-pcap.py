import json,pathlib,struct,tempfile
T=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang/tools');ns={};exec(compile((T/'analyze.py').read_text().split('\nrows=[]')[0],str(T/'analyze.py'),'exec'),ns)
def packet(sport,dport,size):
 ip=bytearray(20);ip[0]=0x45;struct.pack_into('!H',ip,2,40+size);ip[9]=6
 tcp=bytearray(20);struct.pack_into('!HH',tcp,0,sport,dport);tcp[12]=0x50
 return bytes(14)+ip+tcp+bytes(min(size,42))
def write(p,packets):
 with p.open('wb') as f:
  f.write(struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,96,1))
  for sport,dport,size in packets:
   b=packet(sport,dport,size);f.write(struct.pack('<IIII',1,0,len(b),54+size));f.write(b)
with tempfile.TemporaryDirectory(dir=T) as td:
 p=pathlib.Path(td)/'check.pcap';ports={'source':[44000],'target':[5287,5288]}
 write(p,[(55200,5288,1024),(5288,55200,1024),(44000,53000,42),(53000,44000,42),(5288,55200,0)])
 r=ns['pcap'](p,0,2,ports);assert r=={'request_packets':2,'request_bytes':1066,'reply_packets':2,'reply_bytes':1066},r
 write(p,[(5288,55200,0)]);negative=ns['pcap'](p,0,2,ports);assert negative=={},negative
pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang/pcap-check.json').write_text(json.dumps({'positive':r,'ack_only':negative,'result':'pass'},indent=2));print('TCP command port, reciprocal connection, ACK exclusion checks passed')
