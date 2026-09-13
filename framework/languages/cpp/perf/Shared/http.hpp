#pragma once
#include "contracts.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <functional>
#include <thread>
#include <atomic>

namespace perf {
namespace asio=boost::asio;namespace beast=boost::beast;namespace http=beast::http;using tcp=asio::ip::tcp;
struct uri_t {std::string host,port,path;explicit uri_t(const std::string &uri){const auto begin=uri.find("://");if(begin==std::string::npos)throw std::invalid_argument("URI scheme required.");const auto slash=uri.find('/',begin+3);const auto authority=uri.substr(begin+3,slash==std::string::npos?std::string::npos:slash-begin-3);const auto colon=authority.rfind(':');if(colon==std::string::npos)throw std::invalid_argument("Explicit port required.");host=authority.substr(0,colon);port=authority.substr(colon+1);path=slash==std::string::npos?"/":uri.substr(slash);}};
inline json http_json(const std::string &url,const std::string &method,const json &body,int timeout_ms) {const uri_t uri(url);asio::io_context io;tcp::resolver resolver(io);beast::tcp_stream stream(io);stream.expires_after(std::chrono::milliseconds(timeout_ms));stream.connect(resolver.resolve(uri.host,uri.port));http::request<http::string_body> request(method=="POST"?http::verb::post:http::verb::get,uri.path,11);request.set(http::field::host,uri.host);request.set(http::field::content_type,"application/json");if(method=="POST")request.body()=body.dump();request.prepare_payload();http::write(stream,request);beast::flat_buffer buffer;http::response<http::string_body> reply;http::read(stream,buffer,reply);if(reply.result_int()<200||reply.result_int()>=300)throw std::runtime_error("HTTP control failed: "+reply.body());return json::parse(reply.body());}
class http_listener_t {
    asio::io_context io;tcp::acceptor acceptor;std::thread thread;std::atomic_bool stopping=false;
  public:
    using dispatch_t=std::function<std::pair<int,json>(const std::string&,const std::string&,const json&)>;
    http_listener_t(const std::string &url,dispatch_t dispatch):acceptor(io) {const uri_t uri(url);acceptor.open(tcp::v4());acceptor.set_option(tcp::acceptor::reuse_address(true));acceptor.bind({asio::ip::make_address(uri.host),static_cast<unsigned short>(std::stoul(uri.port))});acceptor.listen();acceptor.non_blocking(true);thread=std::thread([this,dispatch=std::move(dispatch)]{while(!stopping){tcp::socket socket(io);boost::system::error_code error;acceptor.accept(socket,error);if(error==asio::error::would_block||error==asio::error::try_again){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}if(error){if(stopping)break;throw boost::system::system_error(error);}beast::tcp_stream stream(std::move(socket));stream.expires_after(std::chrono::seconds(5));beast::flat_buffer buffer;http::request<http::string_body> request;http::read(stream,buffer,request,error);if(error)continue;int status=200;json body;try{auto result=dispatch(std::string(request.method_string()),std::string(request.target()),request.body().empty()?json::object():json::parse(request.body()));status=result.first;body=std::move(result.second);}catch(const std::exception &failure){status=500;body={{"errorType",typeid(failure).name()},{"message",failure.what()}};}http::response<http::string_body> response(static_cast<http::status>(status),11);response.set(http::field::content_type,"application/json");response.keep_alive(false);response.body()=body.dump();response.prepare_payload();http::write(stream,response,error);}});}
    ~http_listener_t(){close();}void close(){stopping=true;if(thread.joinable())thread.join();boost::system::error_code error;acceptor.close(error);}
};
}
