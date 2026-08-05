package systems.zlink.e2e.storefailure.client.support;

import java.time.Duration;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

final class StoreFailureHttpClient implements AutoCloseable {
    private final ZLinkHttpClient http;

    StoreFailureHttpClient(String baseUrl) {
        this.http = ZLinkHttpClient.create(baseUrl).build();
    }

    String get(String path) {
        return requireSuccess(http.get(path)
            .timeout(Duration.ofSeconds(5))
            .submitRaw()
            .toCompletableFuture()
            .join(), path);
    }

    String postJson(String path, Object body) {
        return requireSuccess(http.post(path)
            .timeout(Duration.ofSeconds(10))
            .body(body)
            .submitRaw()
            .toCompletableFuture()
            .join(), path);
    }

    private static String requireSuccess(RawHttpResponse response, String path) {
        if (response.status() < 200 || response.status() >= 300) {
            throw new IllegalStateException(
                "HTTP " + response.status() + " from " + path + ": " + response.body());
        }
        return response.body();
    }

    @Override
    public void close() {
        http.close();
    }
}
