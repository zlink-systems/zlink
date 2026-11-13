import sys,struct,json,collections,pathlib
sys.path.insert(0,'/root/jobs/fx/cx/v21/s033/jio/tools')
from wire import decode
p=pathlib.Path(sys.argv[1]);f=p.open('rb');gh=f.read(24);endian='<' if gh[:4]==b'\xd4\xc3\xb2\xa1' else '>';link=struct.unpack(endian+'I',gh[20:24])[0];assert link==1,link
streams=collections.defaultdict(dict)
while h:=f.read(16):
 sec,usec,n,orig=struct.unpack(endian+'IIII',h);d=f.read(n)
 if d[12:14]!=b'\x08\x00':continue
 ip=d[14:];ihl=(ip[0]&15)*4
 if ip[9]!=6:continue
 tcp=ip[ihl:];sport,dport,seq=struct.unpack('!HHI',tcp[:8]);off=(tcp[12]>>4)*4;data=tcp[off:]
 if data:streams[('.'.join(map(str,ip[12:16])),sport,'.'.join(map(str,ip[16:20])),dport)][seq]=data
res=[]
for key,packets in streams.items():
 merged=bytearray();prev=None;gaps=0
 for seq,data in sorted(packets.items()):
  if prev is None:prev=seq
  if seq>prev:
   gaps+=seq-prev;prev=seq
  cut=max(0,prev-seq);merged.extend(data[cut:]);prev=max(prev,seq+len(data))
 d=decode(merged);hist=collections.Counter(tuple(x['body_bytes'] for x in r) for r in d['records'])
 examples=[];seen=set()
 # Recover full record bytes for header decoding.
 for record in d['records']:
  shape=tuple(x['body_bytes'] for x in record)
  if shape in seen or sum(shape)<1000:continue
  seen.add(shape);examples.append(record)
  for frame in record:
   pre=bytes.fromhex(frame['prefix_hex'])
   at=bytes(merged).find(pre)
   body=bytes(merged[at:at+frame['body_bytes']])
   frame['body_hex']=body.hex()
   if body.startswith(b'ZM\x02'):
    flags,codec=body[4:6];kind=(5 if flags&2 else 2) if body[3]==20 else 1;pos=22;channel=body[pos+1:pos+1+body[pos]].decode();pos+=1+body[pos];name=body[pos+1:pos+1+body[pos]].decode();pos+=1+body[pos]
    frame['descriptor']={'major':2,'operation':body[3],'flags':flags,'kind':kind,'codec':codec,'identity128':body[6:22].hex(),'channel':channel,'message':name,'deadline':None if not flags&1 else int.from_bytes(body[pos:pos+8],'big')}
   elif body[:1] == b'\x01' and len(body)>1000:
    pos=5
    packet=body[pos+1:pos+1+body[pos]].decode();pos+=1+body[pos]
    content=body[pos+1:pos+1+body[pos]].decode();pos+=1+body[pos]
    packed_bytes=int.from_bytes(body[pos:pos+4],'big');pos+=4
    count=int.from_bytes(body[pos:pos+4],'big');pos+=4
    inner=[]
    for i in range(count):
     size=int.from_bytes(body[pos:pos+4],'big');pos+=4
     value=body[pos:pos+size];pos+=size
     part={'bytes':size}
     if value.startswith(b'{'):
      try:part['json']=json.loads(value)
      except ValueError:pass
     inner.append(part)
    frame['wrapper']={'packetName':packet,'contentType':content,'packed_bytes':packed_bytes,'inner_parts':inner,'wrapper_bytes':len(body)-sum(x['bytes'] for x in inner)}
   elif body.startswith(b'{'):
    try:frame['json']=json.loads(body)
    except ValueError:pass
 identities=[{'kind':r[0]['kind'],'identity128':bytes.fromhex(r[0]['prefix_hex'])[6:22].hex()} for r in d['records'] if r and bytes.fromhex(r[0]['prefix_hex']).startswith(b'ZM\x02')]
 res.append({'identities':identities,'connection':key,'bytes':len(merged),'gaps':gaps,'record_hist':[{'part_sizes':k,'count':v,'wire_bytes':sum(k)+8*len(k)+8} for k,v in hist.most_common()],'examples':examples,'resync_bytes':d['resync_bytes']})
(p.parent/'wire.json').write_text(json.dumps(res,indent=2));print(p,[(r['connection'],r['record_hist'][:3]) for r in res])
