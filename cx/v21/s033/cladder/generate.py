from pathlib import Path
import re, json, shutil, hashlib
J=Path('/root/jobs/fx/cx/v21/s033/cladder'); R=Path('/root/worktree/zlink-3867s'); B=R/'framework/bench/grpc/cpp'; F=R/'framework/languages/cpp'
# Preserve production async pump, metrics, phase accounting and validation.
a=(B/'common/bench_async.hpp').read_text().replace('#include <zlink/framework/contracts/dispatch/task.hpp>','')
a=a[:a.index('class completion_signal_t')]+a[a.index('// Continuations may be published'):]
(J/'src/common/bench_async.hpp').write_text(a)
s=(B/'common/bench_stats_server.hpp').read_text(); s=s[:s.index('namespace detail\n{')].replace('#include <zlink/framework.hpp>','')
s+='''
// Job-local POSIX control listener; no Framework dependency in rung 0.
class stats_http_server_t {
 std::vector<int> ports; bench_http_callback_t callback;
 std::vector<int> sockets; std::vector<std::thread> threads; std::atomic_bool stopping{false};
 void serve(int fd) {
  pthread_setname_np(pthread_self(), "ladder-http");
  while (!stopping) {
   pollfd p{fd,POLLIN,0}; if(::poll(&p,1,100)<=0) continue;
   int peer=::accept(fd,nullptr,nullptr); if(peer<0) continue;
   timeval timeout{2,0}; setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
   std::string input; char buffer[8192]; size_t header_end=std::string::npos, length=0;
   while(input.size()<65536) {
    auto n=::recv(peer,buffer,sizeof(buffer),0); if(n<=0)break; input.append(buffer,n);
    header_end=input.find("\\r\\n\\r\\n"); if(header_end==std::string::npos)continue;
    auto at=input.find("Content-Length:"); if(at==std::string::npos)at=input.find("content-length:");
    if(at!=std::string::npos)length=std::stoul(input.substr(at+15));
    if(input.size()>=header_end+4+length)break;
   }
   try {
    std::istringstream first(input);std::string method,path;first>>method>>path;
    auto reply=callback(method,path,header_end==std::string::npos?"":input.substr(header_end+4,length));
    std::string response="HTTP/1.1 "+std::to_string(reply.status)+" OK\\r\\nContent-Type: application/json\\r\\nConnection: close\\r\\nContent-Length: "+std::to_string(reply.json.size())+"\\r\\n\\r\\n"+reply.json;
    size_t sent=0;while(sent<response.size()){auto n=::send(peer,response.data()+sent,response.size()-sent,MSG_NOSIGNAL);if(n<=0)break;sent+=n;}
   } catch(const std::exception &e) { std::fprintf(stderr,"control: %s\\n",e.what()); }
   ::close(peer);
  }
 }
public:
 stats_http_server_t(std::vector<int> p,bench_http_callback_t cb):ports(std::move(p)),callback(std::move(cb)){}
 stats_http_server_t(int p,bench_http_callback_t cb):stats_http_server_t(std::vector<int>{p},std::move(cb)){}
 stats_http_server_t(server_metrics_t &m,int port):stats_http_server_t(port,[&m](auto &method,auto &path,auto &)->bench_http_reply_t {
  if(path=="/bench/stats")return {200,m.snapshot_json()};
  if(path=="/bench/reset" && method=="POST"){m.reset();return {200,"{}"};}
  if(path=="/ready")return {200,"{\\"ready\\":true}"}; return {404,"{}"};}){}
 bool start(){for(int port:ports){int fd=::socket(AF_INET,SOCK_STREAM,0);int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(port);if(bind(fd,(sockaddr*)&addr,sizeof(addr))||listen(fd,16)){::close(fd);return false;}sockets.push_back(fd);threads.emplace_back([this,fd]{serve(fd);});}return true;}
 void stop(){stopping=true;for(auto &t:threads)if(t.joinable())t.join();threads.clear();for(int fd:sockets)::close(fd);sockets.clear();}
 ~stats_http_server_t(){stop();}
};
} // namespace zlink_cpp_bench
#endif
'''
s=s.replace('#include "bench_common.hpp"','#include "bench_common.hpp"\n#include <poll.h>\n#include <pthread.h>')
(J/'src/common/bench_stats_server.hpp').write_text(s)
c=(B/'client/bench_cpp_client.cpp').read_text()
c=c[:c.index('// ---------------------------------------------------------------------------\n// gRPC driver')]+c[c.index('class zlink_raw_driver_t'):]
fw=c[c.index('class framework_driver_t'):c.index('int pattern_window')]
c=c[:c.index('namespace fw = zlink::framework;')]+c[c.index('int pattern_window'):]
c=c.replace('#include <zlink/codecs/protobuf.hpp>','').replace('#include <zlink/framework.hpp>','').replace('#include "bench.grpc.pb.h"','').replace('#include <grpcpp/grpcpp.h>','')
c=c.replace('grpc::Version ()','"not-linked"')
start=c.index('                if (_options.implementation == "grpc-cpp")');end=c.index('\n                const bool ready',start)
c=c[:start]+'''                _driver = std::make_unique<zlink_raw_driver_t> (_options, window, command);
'''+c[end:]
c=c.replace('using clock_t_ =','using json = nlohmann::json;\nusing clock_t_ =')
c=c.replace('        return unlimited_request_window;', '        return std::getenv("LADDER_UNBOUNDED") ? unlimited_request_window : 100;')
c=c.replace('    {\n        _socket =', '    {\n        pthread_setname_np(pthread_self(),"ladder-caller");\n        _socket =',1)
c=c.replace('    std::pair<zlink::message_t, zlink::message_t>','    std::pair<zlink::message_t, zlink::message_t>',1)
# Rung 1 substitutes real production header creation/codec for fixed literal copies.
c=c.replace('        const char *envelope = request_envelope ();','''#if LADDER_CODEC
        auto envelope_value = fw::runtime::messaging::client_call_codec_t{}.create_envelope(
          _command_path ? fw::runtime::messaging::message_kind_t::command : fw::runtime::messaging::message_kind_t::request,
          "bench", "BenchPayload", std::chrono::milliseconds(_options.request_timeout_ms), {}, {}, "application/x-protobuf");
        auto header = fw::runtime::messaging::envelope_codec_t{}.encode_header(envelope_value);
#else
        const char *envelope = request_envelope ();''')
c=c.replace('        const size_t body_size =', '#endif\n        const size_t body_size =')
c=c.replace('        const zlink::message_t &body = reply.back ();','''#if LADDER_CODEC
        fw::runtime::messaging::envelope_codec_t{}.decode_header(reply.front(),false).value();
#endif
        const zlink::message_t &body = reply.back ();''')
c='#if LADDER_CODEC\n#include "runtime/messaging/client_call_codec.hpp"\nnamespace fw = zlink::framework;\n#endif\n'+c
c=c.replace('#include "../common/','#include "common/')
(J/'src/raw-client.cpp').write_text(c)
# Lower-layer driver reuses production Framework benchmark's task/admission loop.
prefix=c[:c.index('class zlink_raw_driver_t')]
prefix=prefix.replace('#if LADDER_CODEC\n#include "runtime/messaging/client_call_codec.hpp"\nnamespace fw = zlink::framework;\n#endif\n','')
prefix='''#include "layers.hpp"
#include <zlink/framework.hpp>
#include <zlink/codecs/protobuf.hpp>
'''+prefix
prefix=prefix.replace('using json = nlohmann::json;','namespace fw = zlink::framework;\nusing payload_t = zlink::framework::bench::withgrpc::BenchPayload;\nusing json = nlohmann::json;')
# The original completion_signal_t is needed only by the Framework-shaped driver.
orig=(B/'common/bench_async.hpp').read_text();sig=orig[orig.index('class completion_signal_t'):orig.index('// Continuations may be published')]
prefix=prefix.replace('namespace\n{', 'namespace zlink_cpp_bench {\n'+sig+'}\nnamespace\n{',1)
constructor=fw.index('    framework_driver_t (');ready=fw.index('    bool await_ready',constructor);run=fw.index('    void run (',ready)
fw=fw[:constructor]+'''    framework_driver_t(const options_t &options,int window,bool command):
      _options(options),_window(window),_command(command),_lower(options.raw_request_endpoint,options.raw_request_rid,options.request_timeout_ms) {
      pthread_setname_np(pthread_self(),"ladder-caller");
    }
    bool await_ready(int timeout_ms) override {
      auto until=clock_t_::now()+std::chrono::milliseconds(timeout_ms);
      while(clock_t_::now()<until){auto p=make_payload(1024,phase_warmup);if(_lower.request(std::move(p)).result())return true;}
      return false;
    }
'''+fw[run:]
a0=fw.index('    // A hosted service'); a1=fw.index('    payload_t make_payload',a0);fw=fw[:a0]+fw[a1:]
a0=fw.index('    template <typename T> auto submit'); a1=fw.index('    template <typename T>\n    void run_tasks',a0)
fw=fw[:a0]+'''    template<typename T> auto submit(payload_t payload) {
       static_assert(!std::is_void_v<T>); return _lower.request(std::move(payload));
    }
'''+fw[a1:]
fw=fw.replace('        if (_command)\n            run_tasks<void> (deadline, size, phase, counters, latency);\n        else\n            run_tasks<payload_t> (deadline, size, phase, counters, latency);','        run_tasks<payload_t> (deadline, size, phase, counters, latency);')
a0=fw.index('    stats_http_server_t _host;');a1=fw.index('    uint32_t _run_id',a0);fw=fw[:a0]+'    lower_requester_t _lower;\n'+fw[a1:]
suffix=c[c.index('int pattern_window'):].replace('std::make_unique<zlink_raw_driver_t>','std::make_unique<framework_driver_t>')
(J/'src/layers-client.cpp').write_text(prefix+fw+suffix)
# Save original target's metrics and binding echo baseline verbatim.
t=(B/'zlink/bench_zlink_cpp_server.cpp').read_text().replace('#include "../common/','#include "common/')
t=t.replace('    zlink::received_t received;', '    pthread_setname_np(pthread_self(),"ladder-receive");\n    zlink::received_t received;',1)
(J/'src/raw-server.cpp').write_text(t)
# Production full cell is copied, not rebuilt or modified.
for binary in ['bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server','bench_cpp_grpc_server']:
 shutil.copy2('/root/jobs/fx/cx/v21/s033/csubmit/build/'+binary,J/'build'/('full-'+binary))
# Reuse exact dependency closure from the existing build, but archive copies live in this job.
ninja=Path('/root/jobs/fx/cx/v21/s033/csubmit/build/build.ninja').read_text()
block=ninja[ninja.index('build bench_cpp_client:'):]; block=block[:block.index('\n\n')]
libs=re.search(r'  LINK_LIBRARIES = (.*)',block)[1].split()
libs=[str(J/'lib'/v) if v.startswith('libzlink_framework') else v for v in libs]
compile_block=ninja[ninja.index('build CMakeFiles/bench_cpp_client.dir/bench_client.cpp.o:'):];compile_block=compile_block[:compile_block.index('\n\n')]
incs=re.search(r'  INCLUDES = (.*)',compile_block)[1].split()
incs=[v for v in incs if 'csubmit/build/bench_proto' not in v]+['-I'+str(J/'src'),'-I'+str(J/'src/common'),'-I'+str(F/'framework/src')]
defs=re.search(r'  DEFINES = (.*)',compile_block)[1].split()
(J/'build-config.json').write_text(json.dumps(dict(libs=libs,includes=incs,defines=defs),indent=2))
print('generated scaffolding; window=100 unless LADDER_UNBOUNDED=1')
