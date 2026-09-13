#pragma once
#include "../Shared/measurement.hpp"

namespace perf {
inline fw::actor_ref_t created_actor(const fw::actor_create_result_t &result) {if(const auto *v=std::get_if<fw::actor_create_existing_t>(&result))return v->actor;if(const auto *v=std::get_if<fw::actor_create_created_t>(&result))return v->actor;throw validation_error("IdentityMismatch","Public Actor manager rejected preparation.");}
inline fw::task_t<void> return_reply(measurement_t &m,fw::route_client_t &route,const request_t &request) {auto reply=m.reply(request);m.direction("send",m.config.workload().at("responsePayloadBytes"));if(request.fields.at("returnSpotId").is_string())co_await route.send_to_spot(fw::spot_id_t(request.fields.at("returnSpotId")),reply).async();else if(request.fields.at("returnChannel").is_string())co_await route.send_to_channel(request.fields.at("returnChannel"),reply).async();else throw validation_error("IdentityMismatch","Send/send request lacks public return address.");}
struct channel_request_handler_t {using request_type=request_t;using reply_type=reply_t;measurement_t &m;explicit channel_request_handler_t(measurement_t &value):m(value){}reply_t handle(const request_t &request){handler_scope_t scope(m);m.observe(request);m.direction("reply",m.config.workload().at("responsePayloadBytes"));return m.reply(request);}};
struct channel_probe_handler_t:channel_request_handler_t {using request_type=probe_t;using reply_type=reply_t;using channel_request_handler_t::channel_request_handler_t;reply_t handle(const probe_t &request){return channel_request_handler_t::handle(request_t{request.fields});}};
struct channel_send_handler_t {measurement_t &m;fw::route_client_t &route;channel_send_handler_t(measurement_t &value,fw::route_client_t &outbound):m(value),route(outbound){}fw::task_t<void> handle(const request_t &request){handler_scope_t scope(m);m.observe(request);if(m.config.correlated())co_await return_reply(m,route,request);else m.delivered(request);}};
struct channel_return_handler_t {measurement_t &m;explicit channel_return_handler_t(measurement_t &value):m(value){}void handle(const reply_t &reply){m.receive_reply(reply);}};
struct fanout_handler_t {
    using event_type=event_t;static constexpr const char *topic_name="perf.echo";measurement_t &m;
    explicit fanout_handler_t(measurement_t &value):m(value){}
    void handle(const event_t &event){handler_scope_t scope(m);try{const auto &v=event.fields;
        if(v.at("runId")!=m.config.value.at("runId")||v.at("cellId")!=m.config.value.at("cellId")||v.at("topic")!="perf.echo"||v.at("clockDomainId").get<std::string>().empty())throw validation_error("IdentityMismatch","Fanout identity differs.");
        decimal(v.at("sequence"));decimal(v.at("resetSeq"));signed_decimal(v.at("sentTicks"));validate_payload(v,m.patterns.at(m.config.workload().at("sendPayloadBytes").get<int>()));
        std::lock_guard lock(m.mutex);const std::string phase=v.at("phase");if((phase!="warmup"&&phase!="measured")||(phase=="warmup"&&v.at("resetSeq")!="0")||(phase=="measured"&&v.at("resetSeq")!=m.reset_seq))throw validation_error("PhaseMismatch","Fanout phase/reset differs.");
        if(phase=="warmup"&&m.reset_seq!="0"){m.bump("load.lateWarmupMessages");return;}
        if(m.setup.empty())m.setup.push_back({{"kind","warmupMarker"},{"source","public typed fanout handler"},{"observedValue",v.at("sequence")}});
        auto request=v;request["clientId"]=0;m.delivered(request_t{request});
    }catch(const std::exception &error){m.error(error);throw;}}
};

class actor_t final:public fw::actor_t {fw::actor_context_t context_;public:explicit actor_t(fw::actor_context_t context):context_(std::move(context)){}fw::actor_context_t &context() noexcept override{return context_;}const fw::actor_context_t &context() const noexcept override{return context_;}};
class actor_factory_t final:public fw::actor_factory_t<actor_t> {public:fw::task_t<std::shared_ptr<actor_t>> create(fw::actor_context_t context,std::stop_token) override{co_return std::make_shared<actor_t>(std::move(context));}};
class entry_t final:public fw::entry_spot_t<actor_t> {
    fw::entry_spot_context_t context_;measurement_t &m;fw::route_client_t &route;
  public:
    entry_t(fw::entry_spot_context_t context,measurement_t &value,fw::route_client_t &client):context_(std::move(context)),m(value),route(client){}
    fw::entry_spot_context_t &context() noexcept override{return context_;}const fw::entry_spot_context_t &context() const noexcept override{return context_;}
    void configure() override{context_.handlers().add_actor_request<&entry_t::request>().add_actor_request<&entry_t::probe>();if(m.config.send()||m.config.correlated())context_.handlers().add_actor_send<&entry_t::send>();}
    reply_t request(actor_t &,fw::message_context_t &,const request_t &value){handler_scope_t scope(m);m.observe(value);m.direction("reply",m.config.workload().at("responsePayloadBytes"));return m.reply(value);}
    reply_t probe(actor_t &actor,fw::message_context_t &context,const probe_t &value){return request(actor,context,request_t{value.fields});}
    fw::task_t<void> send(actor_t &,fw::message_context_t &,const request_t &value){handler_scope_t scope(m);m.observe(value);if(m.config.correlated())co_await return_reply(m,route,value);else m.delivered(value);}
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join(std::string_view,const fw::message_t &) override{co_return fw::spot_actor_join_result_t::accept();}
    fw::task_t<void> on_actor_joined(actor_t &) override{co_return;}fw::task_t<void> on_leave_actor(actor_t &) override{co_return;}
};

class spot_t final:public fw::spot_t<actor_t> {
    fw::spot_context_t context_;measurement_t &m;fw::route_client_t &route;
  public:
    spot_t(fw::spot_context_t context,measurement_t &value,fw::route_client_t &client):context_(std::move(context)),m(value),route(client){}
    fw::spot_context_t &context() noexcept override{return context_;}const fw::spot_context_t &context() const noexcept override{return context_;}
    void configure() override{if(m.config.scenario()=="spot-no-await-echo")context_.handlers().add_handler<&spot_t::immediate>();else if(m.config.driven()||(!m.config.send()&&!m.config.correlated()))context_.handlers().add_handler<&spot_t::echo>();else context_.handlers().add_handler<&spot_t::sent>();context_.handlers().add_handler<&spot_t::probe>();if(m.config.driven())context_.handlers().add_handler<&spot_t::drive>();if(m.config.correlated())context_.handlers().add_handler<&spot_t::returned>();}
    reply_t probe(const probe_t &value){handler_scope_t scope(m);m.observe(request_t{value.fields});return m.reply(request_t{value.fields});}
    reply_t immediate(const request_t &request){handler_scope_t scope(m);m.observe(request);{std::lock_guard lock(m.mutex);m.bump("spot.applicationHandlerEntries");}m.direction("reply",m.config.workload().at("responsePayloadBytes"));return m.reply(request);}
    fw::task_t<reply_t> echo(const request_t &request){handler_scope_t scope(m);m.observe(request);{std::lock_guard lock(m.mutex);m.bump("spot.applicationHandlerEntries");}
        if(m.config.worker()){const auto task_millis=m.config.workload().at("workerTaskMillis").get<int>();const auto domain=m.domain;auto call=context_.run_cpu_worker([task_millis,domain](std::stop_token token){const auto begin=ticks();std::int64_t ended=begin;std::uint32_t x=0x12345678;std::uint64_t iterations=0;do{for(int i=0;i<1024;++i){x^=x<<13;x^=x>>17;x^=x<<5;}iterations+=1024;if(token.stop_requested())throw std::system_error(std::make_error_code(std::errc::operation_canceled));ended=ticks();}while(ended-begin<task_millis*1000000LL);return json{{"startedTicks",std::to_string(begin)},{"endedTicks",std::to_string(ended)},{"clockDomainId",domain},{"iterations",std::to_string(iterations)},{"checksum",x}};});call.timeout(std::chrono::milliseconds(m.config.timeout()));if(m.config.yielding()){std::lock_guard lock(m.mutex);m.bump("spot.applicationYieldCalls");}const auto submitted=ticks();auto observation=m.config.yielding()?co_await call.yield():co_await call.async();const auto continued=ticks();const auto worker_started=std::stoll(observation.at("startedTicks").get<std::string>()),worker_ended=std::stoll(observation.at("endedTicks").get<std::string>());if(observation.at("clockDomainId")!=m.domain||decimal(observation.at("iterations"))<1024)throw validation_error("IdentityMismatch","Worker clock/result differs.");m.record("workerCallLatencyMs",continued-submitted,continued);m.record("workerSubmitToStartMs",worker_started-submitted,continued);m.record("workerTaskLatencyMs",worker_ended-worker_started,continued);m.record("workerResultToContinuationMs",continued-worker_ended,continued);observation["callerSubmittedTicks"]=std::to_string(submitted);observation["continuationTicks"]=std::to_string(continued);observation["clockEvidence"]="Callback and caller use std::chrono::steady_clock in this process.";{std::lock_guard lock(m.mutex);m.worker_observation=std::move(observation);}}
        if(m.config.correlated()&&!m.config.driven())co_await return_reply(m,route,request);else if(m.config.send()&&!m.config.driven())m.delivered(request);else m.direction("reply",m.config.workload().at("responsePayloadBytes"));co_return m.reply(request);
    }
    fw::task_t<void> sent(const request_t &request){handler_scope_t scope(m);m.observe(request);{std::lock_guard lock(m.mutex);m.bump("spot.applicationHandlerEntries");}if(m.config.correlated())co_await return_reply(m,route,request);else m.delivered(request);}
    void returned(const reply_t &reply){m.receive_reply(reply);}
    fw::task_t<drive_reply_t> drive(const drive_t &drive){request_t request{drive.fields.at("echo")};std::shared_ptr<operation_t> op;
        {std::lock_guard lock(m.mutex);auto found=m.operations.find(request.fields.at("correlationId"));if(found!=m.operations.end())op=found->second;
            if(!op||op->done||!m.can_issue()){m.bump("driver.notStarted");co_return drive_reply_t{{{"started",false},{"echo",nullptr}}};}m.bump("spot.applicationHandlerEntries");}
        handler_scope_t scope(m);m.observe(request);
        {std::lock_guard lock(m.mutex);const auto started=ticks();if(op->done||!m.running||m.sealed||started>=m.end){m.bump("driver.notStarted");co_return drive_reply_t{{{"started",false},{"echo",nullptr}}};}op->primary_started=started;request.fields["sentTicks"]=std::to_string(op->primary_started);op->request=request;op->primary=true;m.bump("messages.sent",1,started);m.attempted.add(request.fields.at("clientId"),decimal(request.fields.at("sequence")));}
        try{auto outbound=context_.outbound();if(m.config.send()||m.config.correlated()){
                request.fields["returnSpotId"]=m.config.correlated()?json(context_.spot_id()):json(nullptr);
                {std::lock_guard lock(m.mutex);op->request=request;if(m.config.correlated())m.correlate(op);}
                m.direction("send",m.config.request_bytes());const auto at=ticks();co_await outbound.send_to_channel(m.config.text("channelName"),request).async();m.admission(request,at);
                if(!m.config.correlated())m.finish(op);co_return drive_reply_t{{{"started",true},{"echo",nullptr}}};}
            m.direction("request",m.config.request_bytes());auto call=outbound.request_to_channel(m.config.text("channelName"),request);call.timeout(std::chrono::milliseconds(m.config.timeout()));
            if(m.config.yielding()){std::lock_guard lock(m.mutex);m.bump("spot.applicationYieldCalls");}
            auto reply=m.config.yielding()?co_await call.yield<reply_t>():co_await call.async<reply_t>();m.validate_reply(request,reply);m.finish(op);
            co_return drive_reply_t{{{"started",true},{"echo",reply.fields}}};
        }catch(const std::exception &error){std::lock_guard lock(m.mutex);m.pending.erase(request.fields.at("correlationId"));m.terminal[request.fields.at("correlationId")]=false;m.finish(op,&error);throw;}
    }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join(std::string_view,const fw::message_t &) override{co_return fw::spot_actor_join_result_t::accept();}fw::task_t<void> on_actor_joined(actor_t &) override{co_return;}fw::task_t<void> on_leave_actor(actor_t &) override{co_return;}
};

class session_t:public fw::packet_stream_session_t {
    measurement_t &m;fw::actor_manager_t *actors;std::optional<fw::session_actor_t> actor;
  public:
    using dependencies=std::tuple<measurement_t,fw::actor_manager_t>;
    session_t(measurement_t &value,fw::actor_manager_t &manager):m(value),actors(&manager){}
    explicit session_t(measurement_t &value):m(value),actors(nullptr){}
    fw::task_t<void> on_connected(fw::stream_t &) override{co_return;}fw::task_t<void> on_disconnected(fw::stream_t &) override{co_return;}fw::task_t<void> on_error(fw::stream_t &,const fw::stream_error_t &error) override{throw std::runtime_error(std::string(error.message()));co_return;}
    fw::task_t<void> on_packet(fw::stream_t &stream,const fw::session_message_context_t &context,const fw::message_t &payload) override {
        if(context.packet_name==bind_t::packet_name){
            const auto bind=payload.decode<bind_t>();if(bind.fields.at("runId")!=m.config.value.at("runId")||bind.fields.at("cellId")!=m.config.value.at("cellId"))throw validation_error("IdentityMismatch","Session bind differs.");
            const int id=bind.fields.at("clientId");const std::string actor_id=m.config.value.at("actorIds").at(id);
            auto create=actors->get_or_create(fw::actor_id_t(actor_id),"perf-actor");create.in_mesh(m.config.text("meshName"));create.timeout(std::chrono::milliseconds(m.config.workload().at("setupTimeoutMs").get<int>()));
            auto created=co_await create.async();const auto reference=created_actor(created);auto binding=stream.actors().bind_or_get(reference);actor=co_await binding.async();
            bind_reply_t reply;reply.fields["actorId"]=actor_id;reply.fields["bound"]=true;const auto message=fw::message_t::from(std::move(reply));auto send=stream.reply_packet(message);co_await send.async();
            std::lock_guard lock(m.mutex);m.setup.push_back({{"kind","sessionBind"},{"source","public Actor manager and session bind_or_get"},{"observedValue",actor_id}});co_return;
        }
        if(m.config.actor()){
            if(!actor)throw validation_error("IdentityMismatch","Session is unbound.");m.direction("request",m.config.request_bytes());auto relay=actor->relay_request(context.packet_name,payload);auto reply=co_await relay.async();auto send=stream.reply_packet(reply);co_await send.async();
        }else{
            handler_scope_t scope(m);const auto request=payload.decode<request_t>();m.observe(request);m.direction("reply",m.config.workload().at("responsePayloadBytes"));const auto message=fw::message_t::from(m.reply(request));auto send=stream.reply_packet(message);co_await send.async();
        }
    }
};
class baseline_session_t final:public session_t {public:explicit baseline_session_t(measurement_t &value):session_t(value){}};
}
