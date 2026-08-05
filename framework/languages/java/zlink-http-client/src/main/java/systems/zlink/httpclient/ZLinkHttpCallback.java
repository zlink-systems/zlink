/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

@FunctionalInterface
public interface ZLinkHttpCallback<T> {
    void complete(Throwable error, HttpResponse<T> response);
}
