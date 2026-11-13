from pathlib import Path
import sys
p=Path(sys.argv[1])/'core/src/runtime/core/pipe_write.cpp'
s=p.read_text()
p.with_suffix('.cpp.original4163').write_text(s)
s=s.replace('using zlink::pipe_detail::consume_if_delimiter;', '''namespace {
struct probe4163_t {
    unsigned long long records=0, samples=0, queue_sum=0, queue_max=0, cached_sum=0;
    unsigned long long waits=0, refusals=0, wake=0, flushes=0, more=0;
    unsigned long long bins[9]={};
};
thread_local probe4163_t probe4163;
}
using zlink::pipe_detail::consume_if_delimiter;''')
s=s.replace('    _out_active.store (false, std::memory_order_release);\n    _waiting_for_byte_credit.store (true, std::memory_order_release);','    ++probe4163.waits;\n    _out_active.store (false, std::memory_order_release);\n    _waiting_for_byte_credit.store (true, std::memory_order_release);')
s=s.replace('    publish_outbound_frame_unlocked (*msg_, more);','    if (more) ++probe4163.more;\n    publish_outbound_frame_unlocked (*msg_, more);')
s=s.replace('        _out_complete_record_pending = true;\n        _out_incomplete_bytes = 0;', r'''        ++probe4163.records;
        if ((probe4163.records & 1023) == 0) {
            pipe_t *p = get_peer ();
            const uint64_t read = p ? p->_published_msgs_read.load (std::memory_order_acquire) : 0;
            const uint64_t q = new_msgs_written > read ? new_msgs_written-read : 0;
            const uint64_t cached = _peers_msgs_read.load (std::memory_order_acquire);
            ++probe4163.samples;
            probe4163.queue_sum += q;
            probe4163.cached_sum += new_msgs_written > cached ? new_msgs_written-cached : 0;
            if (q > probe4163.queue_max) probe4163.queue_max=q;
            const uint64_t limits[]={16,64,256,1024,2048,4096,8192,16384};
            unsigned bin=0;
            while (bin<8 && q>limits[bin]) ++bin;
            ++probe4163.bins[bin];
            if ((probe4163.records & 1048575) == 0)
                fprintf(stderr,"PROBE4163 records=%llu samples=%llu qsum=%llu qmax=%llu cached_sum=%llu waits=%llu refusals=%llu wake=%llu flush=%llu more=%llu hwm=%llu charge=%llu bins=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                    probe4163.records,probe4163.samples,probe4163.queue_sum,probe4163.queue_max,probe4163.cached_sum,
                    probe4163.waits,probe4163.refusals,probe4163.wake,probe4163.flushes,probe4163.more,
                    (unsigned long long)hwm,(unsigned long long)message_bytes,
                    probe4163.bins[0],probe4163.bins[1],probe4163.bins[2],probe4163.bins[3],probe4163.bins[4],probe4163.bins[5],probe4163.bins[6],probe4163.bins[7],probe4163.bins[8]);
        }
        _out_complete_record_pending = true;
        _out_incomplete_bytes = 0;''')
s=s.replace('            _out_incomplete_bytes = incomplete_before;\n            _out_incomplete_payload_bytes = payload_before;', '            ++probe4163.refusals;\n            _out_incomplete_bytes = incomplete_before;\n            _out_incomplete_payload_bytes = payload_before;')
s=s.replace('    if (sleeping || explicit_reclassify)\n        send_activate_read (peer);','    ++probe4163.flushes;\n    if (sleeping || explicit_reclassify) {\n        ++probe4163.wake;\n        send_activate_read (peer);\n    }')
p.write_text(s)
