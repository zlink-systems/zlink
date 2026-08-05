package systems.zlink.e2e.channelegress.client;

import java.time.Duration;
import systems.zlink.httpclient.HttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

final class ClientHttp {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(10);

    private ClientHttp() {
    }

    static <T> T post(String endpoint, String path, Object request, Class<T> responseType) {
        try (ZLinkHttpClient http = ZLinkHttpClient.create(endpoint).build()) {
            HttpResponse<T> response = http.post(path)
                .timeout(REQUEST_TIMEOUT)
                .body(request)
                .submit(responseType)
                .toCompletableFuture()
                .join();
            if (response.status() < 200 || response.status() >= 300) {
                throw new IllegalStateException(
                    "POST " + path + " returned HTTP " + response.status()
                        + " body=" + response.body());
            }
            return response.body();
        }
    }

    static <T> T get(String endpoint, String path, Class<T> responseType) {
        try (ZLinkHttpClient http = ZLinkHttpClient.create(endpoint).build()) {
            HttpResponse<T> response = http.get(path)
                .timeout(REQUEST_TIMEOUT)
                .submit(responseType)
                .toCompletableFuture()
                .join();
            if (response.status() < 200 || response.status() >= 300) {
                throw new IllegalStateException(
                    "GET " + path + " returned HTTP " + response.status());
            }
            return response.body();
        }
    }

    static void assertTrue(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
