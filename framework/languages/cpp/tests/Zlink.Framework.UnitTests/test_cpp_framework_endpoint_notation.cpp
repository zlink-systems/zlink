/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/*
 * Pins the endpoint-notation policy (doc/plan/endpoint-notation-policy.ko.md
 * §2.2-2.4) for the C++ normalization utility:
 *   - deterministic normalization is a round trip (case, IPv6, leading
 *     zero ports, trailing slash, surrounding whitespace)
 *   - normalization is lossless (userInfo, query, fragment, IPv6 zone id)
 *   - normalization is idempotent
 *   - notationally-different strings for the same endpoint normalize equal
 *   - localhost and 127.0.0.1 remain different endpoints (no DNS resolution)
 *   - non-authority schemes (ipc) are left byte-identical past the scheme
 */

#include "runtime/transport/endpoint_notation.hpp"

#include <cassert>
#include <cstdio>
#include <string>

namespace transport = zlink::framework::runtime::transport;

namespace
{

void expect_eq (const std::string &actual, const std::string &expected, const char *label)
{
    if (actual != expected) {
        std::fprintf (stderr, "%s: expected [%s] got [%s]\n", label, expected.c_str (),
                     actual.c_str ());
        assert (false && "endpoint normalization mismatch");
    }
}

} // namespace

int main ()
{
    /* scheme + host lowercase */
    expect_eq (transport::normalize_endpoint ("TCP://Example.COM:80"),
              "tcp://example.com:80", "scheme+host lowercase");

    /* leading zero port stripped, ":0" preserved as "0" */
    expect_eq (transport::normalize_endpoint ("tcp://host:0080"), "tcp://host:80",
              "leading zero port");
    expect_eq (transport::normalize_endpoint ("tcp://host:0"), "tcp://host:0",
              "port zero stays zero");

    /* trailing slash removed from the path */
    expect_eq (transport::normalize_endpoint ("tcp://host:80/"), "tcp://host:80",
              "trailing slash removed");
    expect_eq (transport::normalize_endpoint ("tcp://host:80///"), "tcp://host:80",
              "multiple trailing slashes removed");

    /* surrounding whitespace trimmed */
    expect_eq (transport::normalize_endpoint ("  tcp://host:80  "), "tcp://host:80",
              "surrounding whitespace trimmed");

    /* IPv6 bracket notation unified; already-bracketed form untouched */
    expect_eq (transport::normalize_endpoint ("tcp://[::1]:80"), "tcp://[::1]:80",
              "already-bracketed IPv6 stable");
    expect_eq (transport::normalize_endpoint ("tcp://[2001:DB8::1]:443"),
              "tcp://[2001:db8::1]:443", "IPv6 hex digits lowercased");

    /* IPv6 zone id preserved verbatim (not lowercased, not dropped) */
    expect_eq (transport::normalize_endpoint ("tcp://[fe80::1%eth0]:80"),
              "tcp://[fe80::1%eth0]:80", "IPv6 zone id preserved");
    expect_eq (transport::normalize_endpoint ("TCP://[FE80::1%Eth0]:0080"),
              "tcp://[fe80::1%Eth0]:80", "IPv6 zone id case preserved under normalization");

    /* losslessness: userInfo, query and fragment survive untouched */
    expect_eq (transport::normalize_endpoint ("TCP://User:Pass@Host:0080/Path/?Q=1#Frag"),
              "tcp://User:Pass@host:80/Path?Q=1#Frag", "userInfo/query/fragment preserved");

    /* ipc:// is a filesystem path, not a host: only the scheme is
     * lowercased, everything after "://" is byte-identical (case,
     * trailing slash and all). */
    expect_eq (transport::normalize_endpoint ("IPC:///tmp/Foo.sock/"), "ipc:///tmp/Foo.sock/",
              "ipc path left byte-identical");

    /* idempotence */
    for (const auto *input : {"TCP://Example.COM:0080/Path/", "tcp://[2001:DB8::1]:443",
                              "IPC:///tmp/Foo.sock/", "tcp://[fe80::1%eth0]:80"}) {
        const auto once = transport::normalize_endpoint (input);
        const auto twice = transport::normalize_endpoint (once);
        expect_eq (twice, once, "idempotence");
    }

    /* different notations for the same endpoint converge */
    assert (transport::normalize_endpoint ("TCP://HOST:0080")
            == transport::normalize_endpoint ("tcp://host:80/"));
    assert (transport::normalize_endpoint ("tcp://[::1]:80")
            == transport::normalize_endpoint ("TCP://[::1]:0080"));

    /* localhost and 127.0.0.1 are never the same endpoint -- no DNS
     * resolution, per policy §2.1. */
    assert (transport::normalize_endpoint ("tcp://localhost:80")
            != transport::normalize_endpoint ("tcp://127.0.0.1:80"));

    /* bracket_ipv6_host: the shared construction-site helper */
    expect_eq (transport::bracket_ipv6_host ("host"), "host", "bracket_ipv6_host plain host");
    expect_eq (transport::bracket_ipv6_host ("::1"), "[::1]", "bracket_ipv6_host bare IPv6");
    expect_eq (transport::bracket_ipv6_host ("[::1]"), "[::1]",
              "bracket_ipv6_host already bracketed");

    return 0;
}
