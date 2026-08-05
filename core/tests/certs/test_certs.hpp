/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Embedded test certificates for SSL testing.
 *
 * These are self-signed certificates generated specifically for testing.
 * DO NOT use these certificates in production.
 *
 * Certificate details:
 *   - CA: Self-signed CA certificate (valid for 10 years)
 *   - Server: Server certificate signed by CA (valid for 1 year)
 *   - Client: Client certificate signed by CA (valid for 1 year)
 *
 * Generated with OpenSSL on 2026-01-12:
 *   # Generate CA key and certificate
 *   openssl genrsa -out ca.key 2048
 *   openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
 *       -subj "/C=US/ST=Test/L=Test/O=ZLink Test CA/CN=ZLink Test CA"
 *
 *   # Generate server key and certificate with SAN
 *   openssl genrsa -out server.key 2048
 *   openssl req -new -key server.key -out server.csr \
 *       -subj "/C=US/ST=Test/L=Test/O=ZLink Test/CN=localhost"
 *   openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
 *       -CAcreateserial -out server.crt -extensions v3_req -extfile server_ext.cnf
 *   # server_ext.cnf contains: subjectAltName = DNS:localhost, IP:127.0.0.1, IP:::1
 *
 *   # Generate client key and certificate
 *   openssl genrsa -out client.key 2048
 *   openssl req -new -key client.key -out client.csr \
 *       -subj "/C=US/ST=Test/L=Test/O=ZLink Test/CN=Test Client"
 *   openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key \
 *       -CAcreateserial -out client.crt
 */

#ifndef __ZLINK_TEST_CERTS_HPP_INCLUDED__
#define __ZLINK_TEST_CERTS_HPP_INCLUDED__

namespace zlink
{
namespace test_certs
{

//  Self-signed CA certificate (ZLink Test CA, valid until 2036-01-10)
static const char *ca_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDlzCCAn+gAwIBAgIUbGLNLbwV7np9Q07zD9ZWvmA+nkAwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwMTEyMTEyMjUzWhcNMzYwMTEwMTEyMjUzWjBbMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDEWMBQGA1UECgwNWkxpbmsgVGVz\n"
  "dCBDQTEWMBQGA1UEAwwNWkxpbmsgVGVzdCBDQTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
  "ggEPADCCAQoCggEBAKHAdjzB5SsoFlce8T4XBvQa0LAbYP9hQ+jcLXSzoF/QDmeP\n"
  "sxGSE1WINM7ZT9BOqNa8OKl7kWWWYS45XeeqrNLVHDQbz9DvUAqUVaSsoxyAxCtV\n"
  "8Zq+F6Zy01qbLXi+Nv1jWz685X9KSc5SCKz9acoOSBU7IOtJKCQ+QM+/x9PMqQeg\n"
  "B+aRNkv+WE4RRLbpQnIGqSiZkUsNI6Z97o2otsHkGa1oVWWXmKqzUAmembVHjiCl\n"
  "Rn9Ut4/HqqopLn/k2m7/Lj62QT6sOcB8ixDe+H4TwDF6sbxgHcs/1sdobys6VsUF\n"
  "gFSJ5Dm33yYBjQmLfxXRaKMxKGukLmAofa+f28sCAwEAAaNTMFEwHQYDVR0OBBYE\n"
  "FO3BqMenuNdTJuCz5tywoNrd11KjMB8GA1UdIwQYMBaAFO3BqMenuNdTJuCz5tyw\n"
  "oNrd11KjMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBADF2GjWc\n"
  "BuvU/3bG2406XNFtl7pb4V70zClo269Gb/SYVrF0k6EXp2I8UQ7cPXM+ueWu8JeG\n"
  "XCbSTRADWxw702VxryCXLIYYMZ5hwF5ZtDGOagZQWSz38UFy2acCRNqY2ijyISQn\n"
  "3M8YtRdeEGOan+gtTC6/xB3IIRX1tFohT35G/wjld8hs6kJVokYhVfKhk4EZKSxH\n"
  "IiHsVaafpjUwm4EkAwCmwAWkOalKijbo5Jdq9h3UNfOn4RblN80FU/jD2cBFP+L8\n"
  "U/Juz13KFa/4NXp9flzUl/1w5o//V1UXUpfYOMsVT8BaP3dV1pa9lDwhoJERyiI1\n"
  "xj0kGsPBIt3nVwE=\n"
  "-----END CERTIFICATE-----\n";

//  Server certificate signed by CA (localhost with SAN, valid until 2027-01-12)
static const char *server_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrTCCApWgAwIBAgIUH3bva6lTINNSQ2BpgpJStZpT5NQwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwMTEyMTEyMzAxWhcNMjcwMTEyMTEyMzAxWjBUMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDETMBEGA1UECgwKWkxpbmsgVGVz\n"
  "dDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n"
  "CgKCAQEAxZ5FpHxoY5JaTfbS3D1nSlz+BdvnrsZ5PqG+P/H1oGXJnY/2MMZGEeUZ\n"
  "SZg9pVn6ZRURyGTwAHN1X+xarpX057pKfqWtHLztj2+WSJLbBfzSzwPdYNMP/h1C\n"
  "MX9zMbui6ui8Tbys1g5IKO/ZEMRN8bVNHOJ4xkK829RzEu6f/4YCuf4Lz+Z1X4en\n"
  "VBi7DGkWRSUiACjlGvVyZ24KHkLCggbAO3HhhyjZ4FwVd9JuE+d2/jm/neUu6HTt\n"
  "J/9d/5GCovUamkuYWn+e62HA1FkpSnXNbgRrkmAkOrliJG1uCqh3btVzuF1c91Jj\n"
  "8wjm0wm23lDeGVrCWExvyFhk3LBFCwIDAQABo3AwbjAsBgNVHREEJTAjgglsb2Nh\n"
  "bGhvc3SHBH8AAAGHEAAAAAAAAAAAAAAAAAAAAAEwHQYDVR0OBBYEFFrMgnC8k4I0\n"
  "XMjURlF0zXV59HJYMB8GA1UdIwQYMBaAFO3BqMenuNdTJuCz5tywoNrd11KjMA0G\n"
  "CSqGSIb3DQEBCwUAA4IBAQCcXiKLN5y7rumetdr55PMDdx+4EV1Wl28fWCOB5nur\n"
  "kFZRy876pFphFqZppjGCHWiiHzUIsZXUej/hBmY+OhsL13ojfGiACz/44OFzqCUa\n"
  "I83V1M9ywbty09zhdqFc9DFfpiC2+ltDCn7o+eF7THUzgDg4fRZYHYM1njZElZaG\n"
  "ecFImsQzqFIpmhB/TfZIZVmBQryYN+V1fl4sUJFiYEOr49RjWnATf6RKY3J5VKHp\n"
  "TWSm7rTd4jB0CvyNlPpS+fYBdGC72m6R3zrce8Scfto+HPH4YdIU5AdoRHCCtOrA\n"
  "Mq9brLTPUzAqlzC7zDw41hI/MS1Cdcxb1dZkKHgMXu8W\n"
  "-----END CERTIFICATE-----\n";

//  Server private key
static const char *server_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDFnkWkfGhjklpN\n"
  "9tLcPWdKXP4F2+euxnk+ob4/8fWgZcmdj/YwxkYR5RlJmD2lWfplFRHIZPAAc3Vf\n"
  "7FqulfTnukp+pa0cvO2Pb5ZIktsF/NLPA91g0w/+HUIxf3Mxu6Lq6LxNvKzWDkgo\n"
  "79kQxE3xtU0c4njGQrzb1HMS7p//hgK5/gvP5nVfh6dUGLsMaRZFJSIAKOUa9XJn\n"
  "bgoeQsKCBsA7ceGHKNngXBV30m4T53b+Ob+d5S7odO0n/13/kYKi9RqaS5haf57r\n"
  "YcDUWSlKdc1uBGuSYCQ6uWIkbW4KqHdu1XO4XVz3UmPzCObTCbbeUN4ZWsJYTG/I\n"
  "WGTcsEULAgMBAAECggEACAoWclsKcmqN71yaf7ZbyBZBP95XW9UAn7byx25UDn5H\n"
  "3woUsgr8nehSyJuIx6CULMKPGVs3lXP4bpXbqyG4CeAss/H+XeekkL5D0nO4IsE5\n"
  "BSBkaL/Wh275kbCA8HyU9gAZkQLkZbPFCb+XCKLfOpntcHWGut2CLs/VVzCLbX1A\n"
  "hHerqJf3qEW+cU1Va5On+A2BEK7XtYFIR6IabS2LN5ecoZUfQ4EoeypdpQPRKwqM\n"
  "m1tSet4CsRfovguLdY5Z/hAhFLZCMKF5zs8zzGln9+S+G5y2fdJ4VxwbeR0OqyAh\n"
  "cB56xJo3L7rLm6hAoIb0mVXaiyRRGEuCBE/t9/pmSQKBgQD2hQgHpC20bQCyh08B\n"
  "1CyJKz1ObZJeYCWR6hE0stUKKq9QizY9Ci8Q1Hg8eEAtKCKjW74DbJ7bgGJBm6rS\n"
  "yNgpZZ3zw6NDSm4wY33y4alB5jzMR+H7izb6vxMPVcXn3DpjzoklxkN4l8JvgTbt\n"
  "KxZWxD3hS+C6NuNKE4LHipJO1wKBgQDNN89O/71ktIBpxiEZk4sKzdq3JZMErFBi\n"
  "cFJ4vATJ1LstrWdOAtOgRqQN81GhCSZ79vybrcOaq4Q4qLzsOWrAo7nb53gq684Y\n"
  "GaVAZfxzA+qECyEY3CzrKnwIbSFvJY+IfA1QL/ricce8oL7lIRIP1+MuhvGUdw55\n"
  "vXs01Wv47QKBgDo1sW60esJW1spRHvvMkPOWzTQetWgphdWNkqCB9cIf0CPRq24A\n"
  "YJq1wOpubqD7ECrIt/ZxCJXGG+1oB48cM8aaoxBzSrLR+XDdnVjjpibUadjGxHq0\n"
  "JbhRs/t0AnY8T2FP3JyZ00a/dv8DYOfhu7WjQwVW+GqgGU1djAz4EJIjAoGBAJe+\n"
  "iOBVYmowvjN4eck7vDiE9xEuC4QNFnNzssfr326Oism/yv94P5voIC7gmJ+G8JoB\n"
  "i9BhsJ2R7fcnbmsOGc3QQwJEKisyqfZQIE16HC2/240/3X1QcTaC96wTZgGVuIin\n"
  "kgCVOeJvV8423nD2/zAP5sDkr4Wkc2O5pHzwwyIRAoGAID2/HQQbczTqQlEAXltB\n"
  "K8YbNLP75FY+9w10SH3B0hUnEP+9YdeHvxkXdWtewn+TjkXnc3AYlb9A9u7GUuB+\n"
  "K2AF/TMl2YdHFOEDtMAZ8IT6womo6JHYj4+FfbxPiMmOfBmOKrdxQ/WrqfCnZwEs\n"
  "Dhpkrp6xWJWSNvXS0XcWGfM=\n"
  "-----END PRIVATE KEY-----\n";

//  Client certificate signed by CA (valid until 2027-01-12)
static const char *client_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDODCCAiACFB9272upUyDTUkNgaYKSUrWaU+TVMA0GCSqGSIb3DQEBCwUAMFsx\n"
  "CzAJBgNVBAYTAlVTMQ0wCwYDVQQIDARUZXN0MQ0wCwYDVQQHDARUZXN0MRYwFAYD\n"
  "VQQKDA1aTGluayBUZXN0IENBMRYwFAYDVQQDDA1aTGluayBUZXN0IENBMB4XDTI2\n"
  "MDExMjExMjMwNloXDTI3MDExMjExMjMwNlowVjELMAkGA1UEBhMCVVMxDTALBgNV\n"
  "BAgMBFRlc3QxDTALBgNVBAcMBFRlc3QxEzARBgNVBAoMClpMaW5rIFRlc3QxFDAS\n"
  "BgNVBAMMC1Rlc3QgQ2xpZW50MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC\n"
  "AQEAp++HS1h1/lTtPzH+J/d+qOR2/9AVSdMVvW3zIeG3oAomnxKZIbMvTw7nH4EC\n"
  "coPgU4Ff3N1kuTmbnqiLH8xxFiwEd54I87DGOOxhWnSoXN2jtdrVUh9kJh4T3N/v\n"
  "XZnUFNrnCLmwaVOSMsZpUcNtaHkT2pzJ7L6x/mEIfTUrCED6uiEGOHNGrpSJp0Bx\n"
  "qxQ/wTBIbzya5T7G6J16ef5eDths2w3kb2iXKRiYYl+ULbKehVqpHJpgBDYYvuWi\n"
  "egtTV6zWDRYuFn0pJSVDFOENbK9DVJine3hExmcFFLumrHz8UPaA1s/iz6OOM05C\n"
  "BzXsi002v0LpmP3C6Mf+n8t/zwIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQAwgu4i\n"
  "Nqti9SRtNUEo+zPnEe7mAAjssp6wGbaHHp2WaNYsSJRazkBQ8Ujfdchf0oQEOKKB\n"
  "1k3qHnhqTSjCQ2AwfTqLyv6jEVJs2JvXHjSKcHIKPrIM1pwviz2WcFgdPoZtGIK/\n"
  "+7D7eukQqqG0xjK3ki1xEaLVciSakWnVl13fVEEjUxhII7cMORhXkdrBhYJ06nbu\n"
  "XHotXyuDDdyB9seyoyBujk7/HWIaO8KmwabChtIJhOkmMZ385GNMFKjI3On1A8bd\n"
  "sSf9nCIqFxITinCExM2Wq/yW4EPLpr2EvqttaThkZA4Z+6tCYUi5czIQGujpX2QG\n"
  "kTTn4KkupzciAMjk\n"
  "-----END CERTIFICATE-----\n";

//  Client private key
static const char *client_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCn74dLWHX+VO0/\n"
  "Mf4n936o5Hb/0BVJ0xW9bfMh4begCiafEpkhsy9PDucfgQJyg+BTgV/c3WS5OZue\n"
  "qIsfzHEWLAR3ngjzsMY47GFadKhc3aO12tVSH2QmHhPc3+9dmdQU2ucIubBpU5Iy\n"
  "xmlRw21oeRPanMnsvrH+YQh9NSsIQPq6IQY4c0aulImnQHGrFD/BMEhvPJrlPsbo\n"
  "nXp5/l4O2GzbDeRvaJcpGJhiX5Qtsp6FWqkcmmAENhi+5aJ6C1NXrNYNFi4WfSkl\n"
  "JUMU4Q1sr0NUmKd7eETGZwUUu6asfPxQ9oDWz+LPo44zTkIHNeyLTTa/QumY/cLo\n"
  "x/6fy3/PAgMBAAECggEAH6R51SAPZ4MgCsrELUqscm1N4MiX4kekLoOzjpxFqaGQ\n"
  "AmQSN2/YR6iystRvnh0sHP/hDLAohOAOavgt8qlmW3uiwdkkraoOx3X+p/kYKhtW\n"
  "9/KKREWG7Mm5C2KkavoLpHxUkOfQDiCBiRCqko3kpQ0/SO9G+tU5m4kz/MeSWqT5\n"
  "EPtaPiMiNIKBPgwLk+jID2xHaDqWTvawWEQNZUc0dEzPKmPIgvfRa8IlsiD0S9FV\n"
  "36Pm8dxRGL6niVe8jlNqrhqfQxBf6pnGhg6qNn4Xtz+Lr12IQOM9bxaRqegcV19Y\n"
  "rMXvaDqqpPbaPhhOyuWR+x3LmOm106+/HHI1BLkBEQKBgQDosKXXTCvuDR6oHb8+\n"
  "aL0hINr/cZ6PiVstuCTPhlDmXn2wtZ2quHjEr3aUSu9qSZPA+HVN352godXfIIkm\n"
  "6L8cKSUTJwCYpvBdYLirwk0tXdhfyDyPcaUCDb3jMoQ9InxG7nCZ/s2Zl4q7qKik\n"
  "SBpOg/K+yNiLheIcbugbtXxIcQKBgQC4wkAQPawVKw9UHiRSU20Ain7//EJz786C\n"
  "IHK+43QMJ91dBEegFJK1wsApAhpBPT3Vfy0tl8jo8v3xVCiNj556IgiRQ6brsEzT\n"
  "u0cdHm6jNwrScmOrMMeHyoVFvSdUfKCQ+nCA2lmb1JuR+aQdkjznZI4wTPZPE+Tv\n"
  "L94ipQZsPwKBgGBGlpeaIKMCMqkEhdhgpcBLQ9FlRWHGRz+HbVOgE2D9v6uZuX6l\n"
  "jPJ0Vu/MgXrMrqGtK3vpBeMskr0bTSQYMNqJ+5kNDiYbDGDWYBJQ9nXK2nfm9Ye0\n"
  "Ub2jyelzQVu6JQmEJnrQ/miKVxCGHCC85IWP+qQNnes8ne19xfORB7dBAoGAGVJW\n"
  "IWOyb/xEz4yKAdZ5O/e/TCowmV8meGMmFs9pmjjkd9kcT+5B5TNZzsUBACv6i4Yw\n"
  "lO0WlgankymrnSsv1yFO90nEWM0C2onyRyVimG/0xb7ztgSrdArnlRVFjKjAAN6y\n"
  "CJbkbR0IbUs/mOXv/u6jJi+GGnRpjfaLhUgEx1UCgYA2CzGC4kuRsVamziVnJPNH\n"
  "0SNabXgd5YtTrzEqjSKPaEMJSn1Ccym+zcIM6HN+WNgN9jsq8suyRaUKx53+UvLA\n"
  "raGF1aCvzA/LftziHMw5DbXTfcMMoE6XRjYykEsWG/nATcVfuS4tImi17aWojBUb\n"
  "Vp/sNGqEoYw8shE9mZTPDA==\n"
  "-----END PRIVATE KEY-----\n";

} // namespace test_certs
} // namespace zlink

#endif // __ZLINK_TEST_CERTS_HPP_INCLUDED__
