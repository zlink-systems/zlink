package systems.zlink.framework.runtime.internal.transport;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.net.InetAddress;
import java.net.UnknownHostException;

/** Resolves the endpoint that a remote process uses for one listener. */
public final class ZLinkListenerIdentity {
    private ZLinkListenerIdentity() {}

    public static String advertisedEndpoint(String boundEndpoint, String advertiseHost) {
        String endpoint = ZLinkEndpointNotation.normalize(boundEndpoint);
        if (endpoint == null) {
            return null;
        }
        if (advertiseHost != null && !advertiseHost.isBlank()) {
            if (isWildcardHost(advertiseHost)) {
                throw new ZLinkConfigurationException(
                        "advertise host must not be a wildcard address");
            }
            return ZLinkEndpointNotation.normalize(
                    ZLinkEndpointNotation.withHost(endpoint, advertiseHost));
        }

        String boundHost = endpointHost(endpoint);
        if ("0.0.0.0".equals(boundHost)) {
            return ZLinkEndpointNotation.withHost(endpoint, "127.0.0.1");
        }
        if (isIpv6Wildcard(boundHost)) {
            return ZLinkEndpointNotation.withHost(endpoint, "::1");
        }
        return endpoint;
    }

    static boolean isWildcardHost(String host) {
        if (host == null) {
            return false;
        }
        String value = unbracket(host.strip());
        return value.equals("*") || value.equals("0.0.0.0") || isIpv6Wildcard(value);
    }

    private static boolean isIpv6Wildcard(String host) {
        if (host == null) {
            return false;
        }
        String value = unbracket(host.strip());
        int scope = value.indexOf('%');
        if (scope >= 0) {
            value = value.substring(0, scope);
        }
        if (value.indexOf(':') < 0) {
            return false;
        }
        try {
            return InetAddress.getByName(value).isAnyLocalAddress();
        } catch (UnknownHostException invalid) {
            return false;
        }
    }

    private static String endpointHost(String endpoint) {
        int schemeEnd = endpoint.indexOf("://");
        if (schemeEnd < 0) {
            return null;
        }
        int hostStart = schemeEnd + 3;
        int authorityEnd = endpoint.length();
        for (int index = hostStart; index < endpoint.length(); index++) {
            if ("/?#".indexOf(endpoint.charAt(index)) >= 0) {
                authorityEnd = index;
                break;
            }
        }
        int at = endpoint.lastIndexOf('@', authorityEnd - 1);
        if (at >= hostStart) {
            hostStart = at + 1;
        }
        if (hostStart < endpoint.length() && endpoint.charAt(hostStart) == '[') {
            int close = endpoint.indexOf(']', hostStart + 1);
            return close < 0 ? null : endpoint.substring(hostStart + 1, close);
        }
        int hostEnd = authorityEnd;
        for (int index = hostStart; index < authorityEnd; index++) {
            char value = endpoint.charAt(index);
            if (value == ':') {
                hostEnd = index;
                break;
            }
        }
        return endpoint.substring(hostStart, hostEnd);
    }

    private static String unbracket(String host) {
        return host.length() >= 2 && host.charAt(0) == '[' && host.charAt(host.length() - 1) == ']'
                ? host.substring(1, host.length() - 1)
                : host;
    }
}
