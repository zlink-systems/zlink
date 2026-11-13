"""Read-only Linux process-memory observations of the unmodified C perf state.
Layout: router_router_recv_state_t in perf_router_router.cpp:20 and
latency_stats_builder_t in perf_single_latency.hpp:90. x86_64 GCC/libstdc++ ABI.
Backlog is a bracket from sender payload sequence minus active receive count;
it includes at most the in-progress send and is not a physical queue counter.
"""
import os,struct,time
class Reader:
 def __init__(self,pid,expected_cap=1000000):
  self.expected_cap=expected_cap;self.pid=pid;self.started_ns=time.monotonic_ns();self.deadlines=[];self.stack_lo=None;self.stack_hi=None;self.fd=os.open(f'/proc/{pid}/mem',os.O_RDONLY);self.state=None;self.payload=None;self.payload_vector=None
 def read(self,addr,size):return os.pread(self.fd,size,addr)
 def locate(self):
  maps=open(f'/proc/{self.pid}/maps').read().splitlines()
  stack=next(x for x in maps if x.endswith('[stack]'));lo,hi=[int(x,16) for x in stack.split()[0].split('-')];data=self.read(lo,hi-lo);self.stack_lo=lo;self.stack_hi=hi
  self.deadlines=[{'addr':lo+i,'value':struct.unpack_from('<Q',data,i)[0]} for i in range(0,len(data)-8,8) if self.started_ns+4500000000 < struct.unpack_from('<Q',data,i)[0] < self.started_ns+5500000000]
  needle=struct.pack('<QQ',1024,1024)+b'\x07ROUTER1';start=0;candidates=[]
  while True:
   off=data.find(needle,start)
   if off<0:break
   s=lo+off-8;start=off+1
   if off>=8 and struct.unpack_from('<I',data,off-8)[0]==1:
    vals=struct.unpack('<QQQ',self.read(s+280,24));cap=struct.unpack('<Q',self.read(s+304,8))[0]
    if cap==self.expected_cap and 0<vals[0]<100000000 and 0<=vals[0]-vals[1]<=2:candidates.append(s)
  if not candidates:return False
  self.state=max(candidates,key=lambda s:struct.unpack('<Q',self.read(s+280,8))[0])
  for off in range(0,len(data)-24,8):
   beg,end,cap=struct.unpack_from('<QQQ',data,off)
   if beg>0x10000 and end-beg==1024 and cap==end:
    try:
     h=self.read(beg,29)
     if struct.unpack_from('<I',h,4)[0]==1 and h[8]==1 and struct.unpack_from('<I',h,9)[0]==1024:
      self.payload=beg;self.payload_vector=lo+off;break
    except OSError:pass
  return True
 def sample(self):
  if (self.state is None or self.payload is None) and not self.locate():return None
  h1=self.read(self.payload,29) if self.payload else None
  data=self.read(self.state+280,64)
  rx,count,total,cap=struct.unpack_from('<QQdQ',data)
  h2=self.read(self.payload,29) if self.payload else None
  r={'state_addr':self.state,'payload_addr':self.payload,'payload_vector_addr':self.payload_vector,'received':rx,'latency_count':count,'sum_ns':total,'sample_cap':cap}
  if h1 and h2:
   seq1=struct.unpack_from('<Q',h1,13)[0];seq2=struct.unpack_from('<Q',h2,13)[0]
   r.update(seq_before=seq1,seq_after=seq2,backlog_lo=seq1-rx,backlog_hi=seq2-rx)
   lo=max(self.stack_lo,self.state-4096);hi=min(self.stack_hi,self.state+4096);area=self.read(lo,hi-lo)
   r['seq_counter_candidates']=[{'addr':lo+i,'value':struct.unpack_from('<Q',area,i)[0]} for i in range(0,len(area)-8,8) if seq1-32 <= struct.unpack_from('<Q',area,i)[0] <= seq2+512 and not self.state <= lo+i < self.state+352]
   r['deadline_candidates']=self.deadlines
  return r
 def close(self):os.close(self.fd)
