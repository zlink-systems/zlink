import ctypes as C, mmap, pathlib, struct, math
FIELDS='socket type closed requests initial bp initial_bp admitted resubmits retry_ok retry_bp retry_other wakes wake_progress wake_no_progress wake_terminal wake_unmatched unexpected_retry completed completion_error all_requests all_completions logical admitted_live underflow overflow delay_n wake_delay_n hwm_reads sndhwm rcvhwm snd_rc rcv_rc hwm_first_snd hwm_first_rcv hwm_min_snd hwm_max_snd hwm_min_rcv hwm_max_rcv begin_seen end_seen reply_calls monitor_rc monitor_abi monitor_detail auto_enabled auto_profile monitor_owned option_sndhwm option_rcvhwm flow_pause_first flow_pause_last flow_resume_first flow_resume_last flow_paused_max blocked_ppm pending_s_max pending_r_max'.split()
class Stats(C.Structure):
    _fields_=[(k,C.c_uint64) for k in FIELDS]+[('delay',C.c_uint64*4096),('wake_delay',C.c_uint64*4096)]
class Output(C.Structure):
    _fields_=[(k,C.c_uint64) for k in 'magic version size pid sockets begin end'.split()]+[('s',Stats*32)]
def read(path, hist=True):
    data=Output.from_buffer_copy(pathlib.Path(path).read_bytes())
    assert data.magic==0x584250434f554e54 and data.size==C.sizeof(Output), (data.magic,data.size,C.sizeof(Output))
    result={k:getattr(data,k) for k in 'version size pid sockets begin end'.split()}
    result['stats']=[]
    for row in data.s[:data.sockets]:
        d={k:getattr(row,k) for k in FIELDS}
        if hist:
            d['delay']={i:n for i,n in enumerate(row.delay) if n}
            d['wake_delay']={i:n for i,n in enumerate(row.wake_delay) if n}
        result['stats'].append(d)
    return result
def quantile(h,p):
    if not h:return None
    threshold=math.ceil(sum(h.values())*p);n=0
    for b,v in sorted((int(i),v) for i,v in h.items()):
        n+=v
        if n>=threshold:
            k=b//64;j=b%64
            return [(2**k)*(1+j/64)/1000,(2**k)*(1+(j+1)/64)/1000]

class SampleView:
    def __init__(self,path):
        self.file=open(path,'rb');self.map=mmap.mmap(self.file.fileno(),0,access=mmap.ACCESS_READ)
    def sample(self):
        n=struct.unpack_from('Q',self.map,32)[0]
        rows=[]
        for i in range(n):
            base=Output.s.offset+i*C.sizeof(Stats)
            rows.append({k:struct.unpack_from('Q',self.map,base+getattr(Stats,k).offset)[0] for k in ['socket','logical','admitted_live']})
        return rows
    def close(self):self.map.close();self.file.close()
