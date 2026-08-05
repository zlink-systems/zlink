#ifndef ZLINK_CPP_PERF_TLS_HPP
#define ZLINK_CPP_PERF_TLS_HPP

#include "perf_socket_adapter.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace perf
{

inline bool perf_tls_file_exists (const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::exists (path, ec) && std::filesystem::is_regular_file (path, ec);
}

inline bool resolve_perf_tls_dir_from (const std::filesystem::path &start,
                                       std::filesystem::path *out_dir)
{
    if (!out_dir)
        return false;

    std::error_code ec;
    std::filesystem::path cur = start;
    if (cur.empty ())
        return false;

    if (std::filesystem::is_regular_file (cur, ec))
        cur = cur.parent_path ();

    for (int i = 0; i < 16; ++i) {
        if (cur.empty ())
            break;

        const std::filesystem::path repo_candidate =
          cur / "bindings" / "cpp" / "tests" / "certs" / "gen";
        if (perf_tls_file_exists (repo_candidate / "server.crt")
            && perf_tls_file_exists (repo_candidate / "server.key")
            && perf_tls_file_exists (repo_candidate / "ca.crt")) {
            *out_dir = repo_candidate;
            return true;
        }

        const std::filesystem::path local_candidate = cur / "tests" / "certs" / "gen";
        if (perf_tls_file_exists (local_candidate / "server.crt")
            && perf_tls_file_exists (local_candidate / "server.key")
            && perf_tls_file_exists (local_candidate / "ca.crt")) {
            *out_dir = local_candidate;
            return true;
        }

        const std::filesystem::path parent = cur.parent_path ();
        if (parent == cur)
            break;
        cur = parent;
    }

    return false;
}

inline bool try_resolve_tls_paths (std::string &cert_out,
                                   std::string &key_out,
                                   std::string &ca_out,
                                   bool verbose = false)
{
    cert_out.clear ();
    key_out.clear ();
    ca_out.clear ();

    std::filesystem::path dir;
    if (!resolve_perf_tls_dir_from (std::filesystem::current_path (), &dir)) {
        const std::filesystem::path exe_probe ("/proc/self/exe");
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink (exe_probe, ec);
        if (ec || !resolve_perf_tls_dir_from (exe, &dir)) {
            if (verbose) {
                std::cerr << "TLS cert path not found: "
                             "bindings/cpp/tests/certs/gen"
                          << std::endl;
            }
            return false;
        }
    }

    cert_out = (dir / "server.crt").string ();
    key_out = (dir / "server.key").string ();
    ca_out = (dir / "ca.crt").string ();
    return true;
}

template <typename SocketLike>
inline bool setup_tls_server (SocketLike &socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    std::string cert;
    std::string key;
    std::string ca;
    if (!try_resolve_tls_paths (cert, key, ca, true))
        return false;

    try {
        socket.set_tls_server (cert, key, false);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
    return true;
}

template <typename SocketLike>
inline bool setup_tls_client (SocketLike &socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    std::string cert;
    std::string key;
    std::string ca;
    if (!try_resolve_tls_paths (cert, key, ca, true))
        return false;

    try {
        socket.set_tls_client (ca, "localhost", false);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
    return true;
}

} // namespace perf

#endif
