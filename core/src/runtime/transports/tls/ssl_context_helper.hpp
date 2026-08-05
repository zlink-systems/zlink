/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SSL_CONTEXT_HELPER_HPP_INCLUDED__
#define __ZLINK_SSL_CONTEXT_HELPER_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include "core/options.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <string>

namespace zlink
{

//  SSL context helper for creating and configuring SSL contexts.
//
//  This class provides helper methods to create SSL contexts with
//  various configurations (server, client) and load certificates
//  from files or PEM strings.
//
//  Thread Safety:
//    - ssl_context objects are NOT thread-safe
//    - Create one context per engine or per connection

class ssl_context_helper_t
{
  public:
    //  Verification modes
    enum verification_mode
    {
        verify_none = 0,  //  No verification
        verify_peer = 1,  //  Verify peer certificate
        verify_fail = 2,  //  Fail if no peer certificate
        verify_client = 3 //  Verify client certificate (server mode)
    };

    //  Create a server SSL context with certificate and private key.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_server_context (const std::string &cert_chain_file,
                           const std::string &private_key_file,
                           const std::string &password = std::string ());

    //  Create a server SSL context with PEM certificate and key strings.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_server_context_from_pem (const std::string &cert_chain_pem,
                                    const std::string &private_key_pem,
                                    const std::string &password = std::string ());

    //  Create a client SSL context.
    //  If ca_cert_file is empty, uses system CA store.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_client_context (const std::string &ca_cert_file = std::string (),
                           bool trust_system = true,
                           verification_mode mode = verify_peer);

    //  Create a client SSL context with PEM CA certificate string.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_client_context_from_pem (const std::string &ca_cert_pem,
                                    bool trust_system = true,
                                    verification_mode mode = verify_peer);

    //  Create a client SSL context with client certificate for mutual TLS.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_client_context_with_cert (const std::string &ca_cert_file,
                                     const std::string &client_cert_file,
                                     const std::string &client_key_file,
                                     const std::string &password = std::string (),
                                     bool trust_system = true,
                                     verification_mode mode = verify_peer);

    //  Create a client SSL context with client certificate from PEM strings.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_client_context_with_cert_from_pem (const std::string &ca_cert_pem,
                                              const std::string &client_cert_pem,
                                              const std::string &client_key_pem,
                                              const std::string &password = std::string (),
                                              bool trust_system = true,
                                              verification_mode mode = verify_peer);

    //  Create a client SSL context from socket TLS options.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_client_context_from_options (const options_t &options, const std::string &hostname);

    //  Create a server SSL context from socket TLS options.
    //  Returns nullptr on failure.
    static std::unique_ptr<boost::asio::ssl::context>
    create_server_context_from_options (const options_t &options);

    //  Configure verification on an existing context
    static bool configure_verification (boost::asio::ssl::context &ctx, verification_mode mode);

    //  Load CA certificate from file into context
    static bool load_ca_certificate (boost::asio::ssl::context &ctx,
                                     const std::string &ca_cert_file);

    //  Load CA certificate from PEM string into context
    static bool load_ca_certificate_from_pem (boost::asio::ssl::context &ctx,
                                              const std::string &ca_cert_pem);

    //  Set hostname verification callback for RFC2818 verification
    //  This enables hostname verification against the server's certificate
    static bool set_hostname_verification (boost::asio::ssl::context &ctx,
                                           const std::string &hostname);

    //  Configure server verification mode based on client cert requirement
    //  If require_client_cert is true, sets verify_peer | verify_fail_if_no_peer_cert
    //  If false, sets verify_none (optional client cert)
    static bool configure_server_verification (boost::asio::ssl::context &ctx,
                                               bool require_client_cert);

    //  Get OpenSSL error string
    static std::string get_ssl_error_string ();
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL

#endif // __ZLINK_SSL_CONTEXT_HELPER_HPP_INCLUDED__
