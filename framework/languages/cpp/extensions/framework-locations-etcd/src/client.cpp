/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "client.hpp"
#include <charconv>
#include <limits>
#include <stdexcept>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace zlink::framework::etcd::detail
{
error_kind_t classify (const grpc::Status &status)
{
    const auto &text = status.error_message ();
    const auto code = status.error_code ();
    if (code == grpc::StatusCode::OUT_OF_RANGE
        && text.find ("required revision has been compacted") != std::string::npos)
        return error_kind_t::compaction;
    if (code == grpc::StatusCode::RESOURCE_EXHAUSTED
        && text.find ("database space exceeded") != std::string::npos)
        return error_kind_t::no_space;
    if ((code == grpc::StatusCode::UNAUTHENTICATED
         && text.find ("invalid auth token") != std::string::npos)
        || (code == grpc::StatusCode::INVALID_ARGUMENT
            && text.find ("revision of auth store is old") != std::string::npos))
        return error_kind_t::auth;
    return error_kind_t::store_failure;
}

operation_t::operation_t (std::chrono::milliseconds timeout, std::shared_ptr<void> lifetime_owner) :
    lifetime (std::move (lifetime_owner))
{
    deadline = gpr_time_add (gpr_now (GPR_CLOCK_MONOTONIC),
                             gpr_time_from_millis (timeout.count (), GPR_TIMESPAN));
}
bool operation_t::expired () const
{
    return gpr_time_cmp (gpr_now (GPR_CLOCK_MONOTONIC), deadline) >= 0;
}
void operation_t::check () const
{
    if (expired ())
        store_failure ();
}

namespace
{
using certificate_t = std::unique_ptr<X509, decltype (&X509_free)>;
certificate_t certificate (const std::string &pem)
{
    if (pem.size () > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        throw std::invalid_argument ("Invalid Store TLS material.");
    std::unique_ptr<BIO, decltype (&BIO_free)> input (
      BIO_new_mem_buf (pem.data (), static_cast<int> (pem.size ())), BIO_free);
    certificate_t result (
      input ? PEM_read_bio_X509 (input.get (), nullptr, nullptr, nullptr) : nullptr, X509_free);
    if (!result)
        throw std::invalid_argument ("Invalid Store TLS certificate.");
    return result;
}

void validate_tls (const etcd_tls_options_t &tls)
{
    if (!tls.trusted_ca_pem.empty ())
        (void) certificate (tls.trusted_ca_pem);
    if (!tls.client_identity)
        return;
    const auto &identity = *tls.client_identity;
    const auto cert = certificate (identity.certificate_pem);
    if (identity.key_pem.empty ()
        || identity.key_pem.size () > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        throw std::invalid_argument ("Invalid Store TLS private key.");
    std::unique_ptr<BIO, decltype (&BIO_free)> input (
      BIO_new_mem_buf (identity.key_pem.data (), static_cast<int> (identity.key_pem.size ())),
      BIO_free);
    std::unique_ptr<EVP_PKEY, decltype (&EVP_PKEY_free)> key (
      input ? PEM_read_bio_PrivateKey (
                input.get (), nullptr, [] (char *, int, int, void *) { return 0; }, nullptr)
            : nullptr,
      EVP_PKEY_free);
    if (!key || X509_check_private_key (cert.get (), key.get ()) != 1)
        throw std::invalid_argument ("Invalid Store TLS private key.");
}

struct address_t
{
    std::string target;
    bool secure;
};
address_t address (const std::string &endpoint)
{
    const bool secure = endpoint.starts_with ("https://");
    const auto offset = secure ? 8U : 7U;
    if (!secure && !endpoint.starts_with ("http://"))
        throw std::invalid_argument ("Invalid Store endpoint.");
    std::string authority = endpoint.substr (offset);
    if (authority.ends_with ('/'))
        authority.pop_back ();
    if (authority.empty () || authority.find_first_of ("/?#@ \\\t\r\n") != std::string::npos)
        throw std::invalid_argument ("Invalid Store endpoint.");
    std::string_view host (authority), port;
    if (authority.front () == '[') {
        const auto end = authority.find (']');
        if (end == std::string::npos || end == 1)
            throw std::invalid_argument ("Invalid Store endpoint.");
        host = std::string_view (authority).substr (0, end + 1);
        const auto suffix = std::string_view (authority).substr (end + 1);
        if (!suffix.empty ()) {
            if (suffix.front () != ':')
                throw std::invalid_argument ("Invalid Store endpoint.");
            port = suffix.substr (1);
            if (port.empty ())
                throw std::invalid_argument ("Invalid Store endpoint.");
        }
    } else if (const auto colon = authority.find (':'); colon != std::string::npos) {
        host = std::string_view (authority).substr (0, colon);
        port = std::string_view (authority).substr (colon + 1);
        if (port.empty ())
            throw std::invalid_argument ("Invalid Store endpoint.");
    }
    if (host.empty ())
        throw std::invalid_argument ("Invalid Store endpoint.");
    if (!port.empty ()) {
        unsigned value = 0;
        const auto parsed = std::from_chars (port.data (), port.data () + port.size (), value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data () + port.size () || value == 0
            || value > 65535)
            throw std::invalid_argument ("Invalid Store endpoint.");
    } else
        authority += secure ? ":443" : ":80";
    return {authority, secure};
}
}

client_t::client_t (const etcd_location_options_t &options) : _credentials (options.credentials)
{
    if (options.key_namespace.empty () || options.key_namespace.size () > 64
        || options.key_namespace.find ('\0') != std::string::npos
        || !is_valid_utf8 (options.key_namespace) || options.operation_timeout.count () <= 0
        || options.endpoints.empty ())
        throw std::invalid_argument ("Invalid Store options.");
    if (_credentials && (_credentials->user.empty () || _credentials->password.empty ()))
        throw std::invalid_argument ("Invalid Store credentials.");
    std::vector<address_t> addresses;
    for (const auto &endpoint : options.endpoints) {
        auto parsed = address (endpoint);
        if (!addresses.empty () && parsed.secure != addresses.front ().secure)
            throw std::invalid_argument ("Store endpoint schemes must match.");
        addresses.push_back (std::move (parsed));
    }
    if (!addresses.front ().secure && options.tls)
        throw std::invalid_argument ("TLS requires HTTPS endpoints.");
    grpc::SslCredentialsOptions ssl;
    if (options.tls) {
        validate_tls (*options.tls);
        ssl.pem_root_certs = options.tls->trusted_ca_pem;
        if (options.tls->client_identity) {
            const auto &identity = *options.tls->client_identity;
            ssl.pem_private_key = identity.key_pem;
            ssl.pem_cert_chain = identity.certificate_pem;
        }
    }
    grpc::ChannelArguments arguments;
    arguments.SetInt ("grpc.enable_retries", 0);
    arguments.SetInt ("grpc.service_config_disable_resolution", 1);
    arguments.SetMaxSendMessageSize (16 * 1024 * 1024);
    arguments.SetMaxReceiveMessageSize (8 * 1024 * 1024);
    for (const auto &endpoint : addresses) {
        auto channel = grpc::CreateCustomChannel (
          endpoint.target,
          endpoint.secure ? grpc::SslCredentials (ssl) : grpc::InsecureChannelCredentials (),
          arguments);
        _endpoints.push_back (
          {etcdserverpb::KV::NewStub (channel), etcdserverpb::Auth::NewStub (channel)});
    }
    _queue = std::make_shared<queue_t> ();
    _queue->worker = std::thread ([queue = _queue] {
        void *tag;
        bool ok;
        while (queue->queue.Next (&tag, &ok))
            static_cast<tag_t *> (tag)->finish (ok);
    });
}

client_t::~client_t ()
{
    close ();
}
void client_t::start ()
{
    std::lock_guard lock (_queue->mutex);
    if (_queue->closing)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Location Store is closed.");
    ++_queue->active;
}
void client_t::finish ()
{
    std::lock_guard lock (_queue->mutex);
    if (--_queue->active == 0) {
        if (_queue->closing)
            _queue->queue.Shutdown ();
        _queue->idle.notify_all ();
    }
}
void client_t::close ()
{
    if (!_queue)
        return;
    std::unique_lock lock (_queue->mutex);
    if (!_queue->closing) {
        _queue->closing = true;
        if (_queue->active == 0)
            _queue->queue.Shutdown ();
    }
    if (!_queue->worker.joinable ())
        return;
    if (_queue->worker.get_id () == std::this_thread::get_id ()
        || zlink::framework::detail::current_completion_owner () != nullptr) {
        // Every frame and RPC tag retains its operation owner through completion.
        // The worker retains its queue until Next drains, including when a
        // consumer completion destroys the provider with another RPC pending.
        _queue->worker.detach ();
        return;
    }
    _queue->idle.wait (lock, [&] { return _queue->active == 0; });
    lock.unlock ();
    _queue->worker.join ();
}

task_t<void> client_t::authenticate (operation_t operation)
{
    etcdserverpb::AuthenticateRequest request;
    request.set_name (_credentials->user);
    request.set_password (_credentials->password);
    auto response = co_await invoke<etcdserverpb::AuthenticateResponse> (
      request, operation, false,
      [] (endpoint_t &endpoint, auto *context, const auto &query, auto *queue) {
          return endpoint.auth->AsyncAuthenticate (context, query, queue);
      });
    if (!response.status.ok ())
        throw call_failure_t (classify (response.status));
    {
        std::lock_guard lock (_token_mutex);
        _token = response.value.token ();
    }
}
task_t<etcdserverpb::TxnResponse> client_t::txn (etcdserverpb::TxnRequest request,
                                                 operation_t operation)
{
    return call<etcdserverpb::TxnResponse> (
      std::move (request), operation,
      [] (endpoint_t &endpoint, auto *context, const auto &query, auto *queue) {
          return endpoint.kv->AsyncTxn (context, query, queue);
      });
}
task_t<etcdserverpb::RangeResponse> client_t::range (etcdserverpb::RangeRequest request,
                                                     operation_t operation)
{
    return call<etcdserverpb::RangeResponse> (
      std::move (request), operation,
      [] (endpoint_t &endpoint, auto *context, const auto &query, auto *queue) {
          return endpoint.kv->AsyncRange (context, query, queue);
      });
}
} // namespace zlink::framework::etcd::detail
