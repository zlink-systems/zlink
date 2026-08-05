/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.Objects;
import java.util.function.Consumer;

public final class ZLinkHttpServerClient implements AutoCloseable {
    private final ZLinkHttpClient client;
    private final ZLinkHttpExecutionTurn executionTurn;
    private final Consumer<Throwable> errorObserver;

    public ZLinkHttpServerClient(
        ZLinkHttpClient client,
        ZLinkHttpExecutionTurn executionTurn,
        Consumer<Throwable> errorObserver) {
        this.client = Objects.requireNonNull(client, "client");
        this.executionTurn = Objects.requireNonNull(executionTurn, "executionTurn");
        this.errorObserver = Objects.requireNonNull(errorObserver, "errorObserver");
    }

    public static ZLinkHttpServerClient create(ZLinkHttpClient client) {
        return new ZLinkHttpServerClient(
            client, new ZLinkFrameworkHttpExecutionTurn(), Throwable::printStackTrace);
    }

    public ZLinkHttpServerRequestBuilder get(String path) { return request(client.get(path)); }
    public ZLinkHttpServerRequestBuilder post(String path) { return request(client.post(path)); }
    public ZLinkHttpServerRequestBuilder put(String path) { return request(client.put(path)); }
    public ZLinkHttpServerRequestBuilder delete(String path) { return request(client.delete(path)); }
    public ZLinkHttpServerRequestBuilder patch(String path) { return request(client.patch(path)); }
    public ZLinkHttpServerRequestBuilder head(String path) { return request(client.head(path)); }
    public ZLinkHttpServerRequestBuilder options(String path) { return request(client.options(path)); }

    private ZLinkHttpServerRequestBuilder request(ZLinkHttpRequestBuilder request) {
        return new ZLinkHttpServerRequestBuilder(request, executionTurn, errorObserver);
    }

    @Override
    public void close() {
        client.close();
    }
}
