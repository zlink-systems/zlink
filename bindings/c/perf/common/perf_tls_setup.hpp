#ifndef PERF_TLS_SETUP_HPP
#define PERF_TLS_SETUP_HPP

#include "perf_infra.hpp"

#ifndef ZLINK_TLS_CERT
#define ZLINK_TLS_CERT 95
#endif
#ifndef ZLINK_TLS_KEY
#define ZLINK_TLS_KEY 96
#endif
#ifndef ZLINK_TLS_CA
#define ZLINK_TLS_CA 97
#endif
#ifndef ZLINK_TLS_HOSTNAME
#define ZLINK_TLS_HOSTNAME 100
#endif

#include <fstream>
#include <iostream>
#include <string>
#if !defined(_WIN32)
#include <unistd.h>
#include <vector>
#endif

namespace perf_tls_certs
{

static const char *ca_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDlzCCAn+gAwIBAgIUR/zXRG8ehuv+DU7HSxpg8ch7AxMwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwOTE1MDIzODI4WhcNNDYwOTE1MDIzODI4WjBbMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDEWMBQGA1UECgwNWkxpbmsgVGVz\n"
  "dCBDQTEWMBQGA1UEAwwNWkxpbmsgVGVzdCBDQTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
  "ggEPADCCAQoCggEBAMLcka2BjyM0pIaWzJKnOO5yeAeWhwOMbOsLYJTmQxB9eIC8\n"
  "eI0bQVf7PObBdOSHOuKVqzo46n2WOr3lFkjIq9wI/ww+QE0O9a2kYmd7eKXQBLeF\n"
  "VmEuVNQ4WqN4CkcTpSgRxeJHeFrxWAm9HRrkIunI/8KMITx0sXI9BQM2U8JeZkld\n"
  "VP3ai2reH6qx/zBHxPQ4+kodkPbHN8t3SzrV09YhmnAUbp0Ic9Ckpala85HSBfGp\n"
  "/PMbmwWoAj4ok2sPX82+t7UrOOuF9gH8p04wbIaIQkifa35PwmfUMOkSKB819xBl\n"
  "SOwF9G0o9caIpyRIpwCjP9WDS8j0BRX5kad2EjsCAwEAAaNTMFEwHQYDVR0OBBYE\n"
  "FCyGURPMFQ+Jw3orkJAlt2kK7V7iMB8GA1UdIwQYMBaAFCyGURPMFQ+Jw3orkJAl\n"
  "t2kK7V7iMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAIAH4SmC\n"
  "yUZ6K5TkGCcMOOy9W/sOuN/Sz2TvNiXy0smWmzsHGbgmSqnqt/KKV/nv8YyPEleX\n"
  "JwN7Dt8ALlzKfB3/CquFac0elMqcTj3Y/27kVvx+E1st/Gym32D5AAnItLsG6Yc0\n"
  "EEEtqAFC0ls/kv+bomGsAQ3u7OKjVa4Q5VvDxb1Ni1c68kKB/OSNGTVkU0VI7yUj\n"
  "rJobxkQYptNs+LS+Z/rx0ob1PvSYiv1/AHgTuBQWJ7RKf+aDN/iOmWzrQa6llpve\n"
  "S7GykhR+8xE686qgN8xwOf/An/csQnt/9F7t7AXjkdsywLRBiFYSSbwIoRyXTIw8\n"
  "lx+/+brpb7UJedA=\n"
  "-----END CERTIFICATE-----\n";

static const char *server_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrTCCApWgAwIBAgIUb7A7AMPrTUvHCvP9KrIgUzGBurQwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwOTE1MDIzODI4WhcNNDYwOTEwMDIzODI4WjBUMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDETMBEGA1UECgwKWkxpbmsgVGVz\n"
  "dDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n"
  "CgKCAQEAwDUlAkS3oP5N8u9FACGHHhsivQZIjxKKz9Ji+Avg9lFuZ0iKUjpROHT/\n"
  "MgEtzGAXhSuHKkqyYwL7h5tC3f4BfKyxjbDk01nWz/8rjP95HoaMXo+i9a/bJ6lb\n"
  "febOUGmmktA8LzzxRiI6he7xH72zIxdw2R0ZKcutuiErfW/+AbqyDYDLUytMZgSt\n"
  "/ee8jwHQLNeRJpIR49hKFHkQNKtWilzQ7aTt4Cpln4J03EqujmGIXSASrf8cFV8x\n"
  "uwV6yExKtk0HmheoMIYlC6ei9LmABnOw5QSiwHwi+YCU04f2k24r/9a+95qgCg3y\n"
  "j33vgYEAzWjItCUzjBmLfBDzWAEqdwIDAQABo3AwbjAsBgNVHREEJTAjgglsb2Nh\n"
  "bGhvc3SHBH8AAAGHEAAAAAAAAAAAAAAAAAAAAAEwHQYDVR0OBBYEFBH5HRIxKpue\n"
  "/bFS8DDwjYnsQLS5MB8GA1UdIwQYMBaAFCyGURPMFQ+Jw3orkJAlt2kK7V7iMA0G\n"
  "CSqGSIb3DQEBCwUAA4IBAQCMoKcdWymrRxTMtbsm37MZp6JyglUbJ/OCzpKWiIY/\n"
  "niEf+S5U4JLBMvAkeu2j4KsT2CFXGj0l9O1KboEFHb/D9xblYR8nKVOsBQMvGEAM\n"
  "uWbcbjznLEO8di9ZJRbm3aG8w/0q0nndPQLBn+WGiZ/7vpGRRtI2o9b5fqfuUvM6\n"
  "bVjHCN3kZn3FqstBHpNlBGujeC/dUxgLEFXz7bznEe+c888RJFbJod32SKInFSJ6\n"
  "qxGOK+0h0566FVj5Rqa1KXKhHnu7JAYFn/9TRxCFieGyDogp2++oTkbNHe26IQv6\n"
  "LCHni8pSVfBqepP6LQVx/x++Z0th8uVVKKDm5wjWGjY0\n"
  "-----END CERTIFICATE-----\n";

static const char *server_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDANSUCRLeg/k3y\n"
  "70UAIYceGyK9BkiPEorP0mL4C+D2UW5nSIpSOlE4dP8yAS3MYBeFK4cqSrJjAvuH\n"
  "m0Ld/gF8rLGNsOTTWdbP/yuM/3kehoxej6L1r9snqVt95s5QaaaS0DwvPPFGIjqF\n"
  "7vEfvbMjF3DZHRkpy626ISt9b/4BurINgMtTK0xmBK3957yPAdAs15EmkhHj2EoU\n"
  "eRA0q1aKXNDtpO3gKmWfgnTcSq6OYYhdIBKt/xwVXzG7BXrITEq2TQeaF6gwhiUL\n"
  "p6L0uYAGc7DlBKLAfCL5gJTTh/aTbiv/1r73mqAKDfKPfe+BgQDNaMi0JTOMGYt8\n"
  "EPNYASp3AgMBAAECggEAR59GFCtRFd/NYhpA5wSXWeOYtUEzJoUtTrXCBVY/1OmR\n"
  "L1F7oZpzi4slURfZXg/sk8YdjufYw0ZoPibf6uLs4O1lGDxzeEJA5q7aJqdIFdTj\n"
  "V5VEjzKhgoz8N9UayiIkXQ7VbnDSI2U7046vMTm6F/hzJ6RNLSLlsLcNgqeJylCG\n"
  "EtMqDwYznt1k4U0U94eO49w9rDX8MXCCWDnPtywdRZWUD0nTqyLbO0+I7AsWhnJv\n"
  "Hi4mhzi8DP85Ec64QWxDoGYBueoDSEsisSgUhMD6hHgklO3FLtPMto+Qxcde8WTe\n"
  "jU+tPzV3YZOB1twjlrrFh6Zl/IQt8kJy5WcI0zCQGQKBgQD38yVoI8UwWs6uRI+f\n"
  "15dHxw3JRbKRNl065BtD9u7VtmCD/3ppUXjYjTpTYhk0hdrkvzDL5U3dogeCc+gK\n"
  "bFRfFZHCTO5O7EIE3NDcK9ORPB0HdjLqwnx/Ec4D1usfP/oagJK0EIwDx5SW7btw\n"
  "4Le66/KoVXtf/v9Fnz3qwOd8YwKBgQDGcrFhALSxM9zt7WxMmGxtAHB2PoehubTt\n"
  "MjBS+fznoBjsF4A5uKExrkdKKliHkkqDs26aeX4AUmnJqaJm3u9S8x6arzsNU4Dy\n"
  "bC0Vq1+ag72UzVFbG8UWl+j//D6Hy6wBSfKuDBsxq5i30vdVaUFRrg372/D9NzPc\n"
  "F8e9wFXj3QKBgH7SZhKzIRwPhmGKfe/jBOTYwottU92EcgE6RVvpBNZY91rspL8T\n"
  "xfz1l5yos32y7XhM9neD7OTtCGxIPqp+KFWOIcTBNq81lrsH+uhynj9OAQcdBQQg\n"
  "wC76e2ZpWk/cmF9P3jmtsQAJ6E2egV5GApPgNXi2aGl8czM4NSJK0txDAoGAc2kM\n"
  "Y5+ndk71M6Iak8kpdZMF1J60/ockA7ZmiDs+q+5d0CAywF7x0BTM/QL3jZC0qTdX\n"
  "IZt6ffFv+IohGraYdKNTrx4tt6hSm6nx5mJOLWxkev+VSukxi9w483bdXthCZlV9\n"
  "P19nCVIEdRPKJ/AYvsn88/aLhpfuHxftYBtVWDkCgYBTxKJrLl6dIciPyPEqr65b\n"
  "LNqlNSN5pHm3WwKP8Zl4doP6eZV9yvP2eqqU/j/yu9xCPt2SEehI3EvLG3pO3NRB\n"
  "s8PjxWqGiemjYMr3d3Jr7QgaJ8lZ6mmd7NScwxcYexqvU4ZW1YS5OAFfkcWpxnYV\n"
  "1TQ99aQXlcSaltTPhzuqug==\n"
  "-----END PRIVATE KEY-----\n";

} // namespace perf_tls_certs

inline std::string write_temp_cert (const char *content, const std::string &suffix)
{
#if defined(_WIN32)
    char temp_dir[MAX_PATH] = {};
    const DWORD temp_dir_length = GetTempPathA (sizeof (temp_dir), temp_dir);
    if (temp_dir_length == 0 || temp_dir_length >= sizeof (temp_dir))
        return std::string ();

    std::string path (temp_dir, temp_dir_length);
    if (!path.empty () && path[path.size () - 1] != '\\'
        && path[path.size () - 1] != '/')
        path += '\\';

    // Each benchmark process owns its certificate file. This avoids a second
    // process observing a partially rewritten PEM file during a multi run.
    path += "zlink_perf_" + suffix + "_"
            + std::to_string (static_cast<unsigned long> (GetCurrentProcessId ())) + ".pem";

    std::ofstream ofs (path.c_str (), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs)
        return std::string ();
    ofs << content;
    if (!ofs)
        return std::string ();
    return path;
#else
    std::string path = "/tmp/bench_" + suffix + ".pem";
    // Fork-based benchmarks prepare the same certificate in many child
    // processes. Publish a complete file atomically so a peer never reads a
    // path while another child has truncated it and is still writing.
    std::string pending_pattern = path + ".XXXXXX";
    std::vector<char> pending_path (pending_pattern.begin (), pending_pattern.end ());
    pending_path.push_back ('\0');
    const int fd = mkstemp (&pending_path[0]);
    if (fd < 0)
        return std::string ();

    const char *cursor = content;
    size_t remaining = std::strlen (content);
    bool complete = true;
    while (remaining > 0) {
        const ssize_t written = write (fd, cursor, remaining);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            complete = false;
            break;
        }
        cursor += written;
        remaining -= static_cast<size_t> (written);
    }
    if (close (fd) != 0)
        complete = false;
    if (!complete || std::rename (&pending_path[0], path.c_str ()) != 0) {
        std::remove (&pending_path[0]);
        return std::string ();
    }
    return path;
#endif
}

inline bool set_tls_path_option (void *socket,
                                 zlink_option_t option,
                                 const std::string &value,
                                 const char *name)
{
    if (zlink_set_option (socket, option, value.c_str (), value.size ()) == ZLINK_CONFIG_OK)
        return true;

    if (bench_debug_enabled ()) {
        std::cerr << "Failed to set " << name << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    return false;
}

inline bool setup_tls_server (void *socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    static std::string cert_path = write_temp_cert (perf_tls_certs::server_cert_pem, "server_cert");
    static std::string key_path = write_temp_cert (perf_tls_certs::server_key_pem, "server_key");

    if (cert_path.empty () || key_path.empty ()) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to write TLS server certificate files" << std::endl;
        return false;
    }

    if (zlink_set_tls_server (socket, cert_path.c_str (), key_path.c_str (), 0)
        == ZLINK_CONFIG_OK) {
        return true;
    }

    if (zlink_errno () != EFAULT) {
        if (bench_debug_enabled ()) {
            std::cerr << "Failed to set TLS server options: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        }
        return false;
    }

    return set_tls_path_option (socket, ZLINK_OPT_TLS_CERT, cert_path, "ZLINK_OPT_TLS_CERT")
           && set_tls_path_option (socket, ZLINK_OPT_TLS_KEY, key_path, "ZLINK_OPT_TLS_KEY");
}

inline bool setup_tls_client (void *socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    static std::string ca_path = write_temp_cert (perf_tls_certs::ca_cert_pem, "ca_cert");
    static const char *hostname = "localhost";

    if (ca_path.empty ()) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to write TLS client CA file" << std::endl;
        return false;
    }

    if (zlink_set_tls_client (socket, ca_path.c_str (), hostname, 0) == ZLINK_CONFIG_OK)
        return true;

    if (zlink_errno () != EFAULT) {
        if (bench_debug_enabled ()) {
            std::cerr << "Failed to set TLS client options: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        }
        return false;
    }

    const int trust_system = 0;
    return set_tls_path_option (socket, ZLINK_OPT_TLS_CA, ca_path, "ZLINK_OPT_TLS_CA")
           && set_tls_path_option (socket, ZLINK_OPT_TLS_HOSTNAME, std::string (hostname),
                                   "ZLINK_OPT_TLS_HOSTNAME")
           && zlink_set_option (socket, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system,
                                sizeof (trust_system))
                == ZLINK_CONFIG_OK;
}

#endif
