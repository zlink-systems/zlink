/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Embedded test certificates for SSL testing.
 *
 * These are self-signed certificates generated specifically for testing.
 * DO NOT use these certificates in production.
 *
 * Certificate details:
 *   - CA: Self-signed CA certificate (valid for ~20 years)
 *   - Server: Server certificate signed by CA (valid for ~20 years)
 *   - Client: Client certificate signed by CA (valid for ~20 years)
 *
 * Regenerated with core/tests/certs/gen/gen.sh on 2026-09-15 (see that
 * script for the exact openssl invocations and the CA_DAYS/LEAF_DAYS
 * knobs). The previous 1-year leaf certificates would have expired
 * 2027-01-12 and turned CI red independently of any release; these run
 * to 2046 to match the long-lived TLS fixtures used elsewhere in the repo
 * (e.g. framework/languages/node/test/fixtures/tls, valid to 2036).
 */

#ifndef __ZLINK_TEST_CERTS_HPP_INCLUDED__
#define __ZLINK_TEST_CERTS_HPP_INCLUDED__

namespace zlink
{
namespace test_certs
{

//  Self-signed CA certificate (ZLink Test CA, valid until 2046-09-15)
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

//  Server certificate signed by CA (localhost with SAN, valid until 2046-09-10)
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

//  Server private key
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

//  Client certificate signed by CA (valid until 2046-09-10)
static const char *client_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDODCCAiACFG+wOwDD601Lxwrz/SqyIFMxgbq1MA0GCSqGSIb3DQEBCwUAMFsx\n"
  "CzAJBgNVBAYTAlVTMQ0wCwYDVQQIDARUZXN0MQ0wCwYDVQQHDARUZXN0MRYwFAYD\n"
  "VQQKDA1aTGluayBUZXN0IENBMRYwFAYDVQQDDA1aTGluayBUZXN0IENBMB4XDTI2\n"
  "MDkxNTAyMzgyOFoXDTQ2MDkxMDAyMzgyOFowVjELMAkGA1UEBhMCVVMxDTALBgNV\n"
  "BAgMBFRlc3QxDTALBgNVBAcMBFRlc3QxEzARBgNVBAoMClpMaW5rIFRlc3QxFDAS\n"
  "BgNVBAMMC1Rlc3QgQ2xpZW50MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC\n"
  "AQEAwwgpMXmHi89/0JVs3GFEvaKsEPvA0RuheGDXDHNQHYCZsdWgyXM0OsD7ZvxO\n"
  "TOIaWrE5ZmjCvjYKlgijQAH8ttRFRHFkbg37OEpG0ZKkjyn4cEk8trH5M5Pm8OVz\n"
  "QOTS+B6yhZRg5GLd+mIPh1UFI7EA7LNe8Me3cAD3twlGyK2Mxmj5Kg6ZUlf85BLT\n"
  "KTdZpYII73OA5m6tyv1LfUFk6e7AmmRLM+tA0UDiz6XrDbYVrJthNrZK3xqw4RPE\n"
  "0U3mulG7043eRczbJGb57lTqTpvF9yICf7Ier5trCf/YLzlG9ODKs6z1GRvEo2Y2\n"
  "RaZ1RbL71JPcZe41njfoeBdQKwIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQAU40cY\n"
  "7SNvV0AcluF4XpsrzTMuMJJ2dWLBgYXYGZ9HBntkwewpEKwz2to8nZuFEAOELGbF\n"
  "Xf7ZdVAXYYMkmOtHvTVSs1uVWpJHeHVFiSd6OpXWxMBEKLzOtgSV2GFCnGkMbbV3\n"
  "na3j5IYu7sQWJ1ekxYuleRLeVSvhMRWM2lZzRnOevdVKvDHc17XDsBmOBr/mKn+B\n"
  "bjd9ZJHIAJ1dWDRV0MUx49+JtQHnFvW0z6Tr74CBMqNfzegkPf1yiC0QMTAdYgV9\n"
  "pfJx2eg7F2cliW2j42lzzG5u7RXU8QZ5jni1K/UV/kTMpJMwdBwsoT6S2akLS6gP\n"
  "yGR83S7GQhCUKd4c\n"
  "-----END CERTIFICATE-----\n";

//  Client private key
static const char *client_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDDCCkxeYeLz3/Q\n"
  "lWzcYUS9oqwQ+8DRG6F4YNcMc1AdgJmx1aDJczQ6wPtm/E5M4hpasTlmaMK+NgqW\n"
  "CKNAAfy21EVEcWRuDfs4SkbRkqSPKfhwSTy2sfkzk+bw5XNA5NL4HrKFlGDkYt36\n"
  "Yg+HVQUjsQDss17wx7dwAPe3CUbIrYzGaPkqDplSV/zkEtMpN1mlggjvc4Dmbq3K\n"
  "/Ut9QWTp7sCaZEsz60DRQOLPpesNthWsm2E2tkrfGrDhE8TRTea6UbvTjd5FzNsk\n"
  "ZvnuVOpOm8X3IgJ/sh6vm2sJ/9gvOUb04MqzrPUZG8SjZjZFpnVFsvvUk9xl7jWe\n"
  "N+h4F1ArAgMBAAECggEAWhAStb4hUfboVzIpqztfuxK70rvvNqFD14sgw6ccgAM6\n"
  "9lxoe56vp9ImRlCM+AQRl3/vudL220+pY89pU82XHfa4ZfrXHdtm/3+NZIoLY/FF\n"
  "wNSRLOzS33aVVvkeWAFTSzEhz33NoKnnSBCwixY/4VOD0cqjNR2FIDvSKLwn69Mq\n"
  "bM27zdoDuDmdehkLjl8ZacDHjlb+WsPuNsdW1PCNfVDZ8UTMxbz6fbcl6Bpnd8GO\n"
  "u97xuI7bkLPC9EQF6dqrU12LZrskL30d5RIQ5A+SvjqFVErwTfz/RQyRxcCjSidO\n"
  "NFtg7Ly66J6Qxxy3iL86i4wjMop4TiBFJgGMbp3KQQKBgQDqSAh3AxwK0rSW3q6q\n"
  "jHBBHo79kIK2Iuk9rA1FWnqPTvUwqEPHLlCp/rfGGBXW8gQYgLmMSkricmBxyj7V\n"
  "pRap0CF39axwhhDeVwIbO//gbI4WOZxK7kf27aTleNmv5DjztJP8sKizCtLKkN8/\n"
  "XydwQ3m2EuwEIPbNLdKCbE5RZQKBgQDVHKiSs4TZMEhVI7ht2E5N+utRnvhDirnO\n"
  "onF/xbvrNeNfVwkM5jCL55FFDi3kLcE6TNuPBf6OzEjvGgVeToUmvDuCSoFPnPC5\n"
  "uMEwzC27Vdypl5/Mmx1N1tdEg+7LJ2Jgr88G7BiP97z0EbyA+JMoYJGGu2HH9KLK\n"
  "k6ntywFKTwKBgQCf/llpjUIVUhfqAGEgL5BpqEjWeV4KrITEjT7y4ftY6v8fH4pJ\n"
  "+CM2NLGkIsanZ9fMM/yDBillw96BVzDaDkgP6AczOR9uKOBUNu9FUhBIX1oZa3aE\n"
  "5X1X8Krv2zwulpkeNW/q3WoX+4hAtfb//CeezzXLVdjAaRKixGxCjVxe0QKBgQCP\n"
  "fpyDKx1ooHoT2dl1HRCcsTeB3eiMkfxKhlamEh5WYhKXP6N4bAaELYhDVyjPW3+g\n"
  "IeA0jXS58hOp63tx5K0DR/tJ36DyWlo8s4phsRmZ6laKWu3edxNkNiT8nVlMsVgs\n"
  "gjSEFLT1O3qfXNHwhW+Q4HUco09TtAVpIcRpuSdFQQKBgGuDFkH/+n4XpkZXiA1v\n"
  "S9+N+CbUm+9ut2KUkZOKwGWowL58ydClBVRNb+gAzRB5AGhux/l8XVPDbxBKOILU\n"
  "Su8bStWz2TRE1VUIn/S9Shd1TlxbaHi7yOn3qz8OOY7Yefu1CwyDAjyeaszH1/p2\n"
  "L+3UeidD1dbkQ/pjGRE5jqBF\n"
  "-----END PRIVATE KEY-----\n";

} // namespace test_certs
} // namespace zlink

#endif // __ZLINK_TEST_CERTS_HPP_INCLUDED__
