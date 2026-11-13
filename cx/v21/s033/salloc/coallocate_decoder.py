from pathlib import Path
p=Path('/root/worktree/zlink-3867a/framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp')
s=p.read_text();a=s.index('void route_client_t::submit_request_reply_message_erased (');b=s.index('\nrequest_task_t<zlink::message_t> route_client_t::submit_channel_request_reply_message_erased',a)
t=s[a:b]
ls=t.index('        auto decode = [state, transport = transport.value ()');bs=t.index('-> result_t<encoded_payload_t> {',ls)+len('-> result_t<encoded_payload_t> {');be=t.index('        };\n        if (mesh)',bs)
body=t[bs:be].replace('target_owner->to_string ()','target.to_string ()')
pos=t.index('    // Selected transports may borrow this stable target');end=t.index('\n    auto effective_timeout',pos)
struct='''    using reply_parts_t = runtime::messaging::message_parts_t;
    using node_reply_t = result_t<reply_parts_t>;
    struct request_decoder_t
    {
        std::shared_ptr<detail::route_client_state_t> state;
        detail::node_transport_t transport;
        zlink::routing_id_t target;
        std::string packet_name;
        std::string router_channel_id;
        detail::request_callback_t<const result_t<encoded_payload_t> &> deliver;

        result_t<encoded_payload_t> decode (const result_t<reply_parts_t> &reply)
        {'''+body+'''
        }

        result_t<encoded_payload_t> project (const result_t<reply_parts_t> &reply)
        {
            try {
                return decode (reply);
            }
            catch (...) {
                return detail::current_exception_result<encoded_payload_t> ();
            }
        }

        void operator() (const result_t<reply_parts_t> &reply) { deliver (project (reply)); }
        void operator() (const result_t<node_reply_t> &reply)
        {
            auto projected = [&] () -> result_t<encoded_payload_t> {
                try {
                    return project (reply.value ());
                }
                catch (...) {
                    return detail::current_exception_result<encoded_payload_t> ();
                }
            }();
            deliver (projected);
        }
    };
    // The selected transport borrows the target held alongside its physical decoder.
    auto context = std::make_shared<request_decoder_t> (request_decoder_t{
      state, transport.value (), std::move (target_node_rid), std::move (packet_name),
      std::move (router_channel_id), std::move (receiver.complete)});'''
pre=t[:pos]
mid=t[end:ls]
import re
mid=re.sub(r'\bpacket_name\b','context->packet_name',mid)
mid=re.sub(r'\brouter_channel_id\b','context->router_channel_id',mid)
mid=mid.replace('target_owner->to_string ()','context->target.to_string ()')
post=t[be+len('        };\n'):]
old='''            (*mesh)->request (*target_owner, std::move (parts), remaining,
                              receiver.project<result_t<runtime::messaging::message_parts_t>> (
                                [decode = std::move (decode)] (const auto &terminal) {
                                    return decode (terminal.value ());
                                }));'''
assert old in post
post=post.replace(old,'''            detail::request_receiver_t<node_reply_t> physical{
              std::move (receiver.admitted),
              detail::request_callback_t<const result_t<node_reply_t> &>::from_shared (context),
              std::move (receiver.fail_public)};
            (*mesh)->request (context->target, std::move (parts), remaining, std::move (physical));''')
old='''            (*route)->request_reply_parts (
              *target_owner, std::nullopt, std::move (parts), deadline,
              receiver.project<runtime::messaging::message_parts_t> (std::move (decode)));'''
assert old in post
post=post.replace(old,'''            detail::request_receiver_t<reply_parts_t> physical{
              std::move (receiver.admitted),
              detail::request_callback_t<const result_t<reply_parts_t> &>::from_shared (context),
              std::move (receiver.fail_public)};
            (*route)->request_reply_parts (context->target, std::nullopt, std::move (parts), deadline,
                                            std::move (physical));''')
t=pre+struct+mid+post
assert 'target_owner' not in t
s=s[:a]+t+s[b:];p.write_text(s)
