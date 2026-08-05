/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

/**
 * Version identity for outgoing requests. This constant is the single source of truth for the
 * Java client version: the Gradle module version and the User-Agent product token
 * ({@code zlink-http-client/<major.minor>}) are both derived from it.
 */
public final class HttpClientVersion {

    public static final String VERSION = "0.3.1";

    public static final String USER_AGENT =
        "zlink-http-client/" + VERSION.substring(0, VERSION.lastIndexOf('.'));

    private HttpClientVersion() {
    }
}
