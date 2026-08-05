package systems.zlink.e2e.toactormessaging.client;

import java.net.URI;
import java.time.Duration;
import systems.zlink.httpclient.HttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

final class ToActorHttpClient {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(35);

    private ToActorHttpClient() {
    }

    static <T> T postJson(String url, Object body, Class<T> replyType) {
        RequestTarget target = target(url);
        try (ZLinkHttpClient http = ZLinkHttpClient.create(target.baseUrl()).build()) {
            HttpResponse<T> response = http.post(target.path())
                .timeout(REQUEST_TIMEOUT)
                .body(body)
                .submit(replyType)
                .toCompletableFuture()
                .join();
            return requireSuccess(response, url);
        }
    }

    static <T> T getJson(String url, Class<T> replyType) {
        RequestTarget target = target(url);
        try (ZLinkHttpClient http = ZLinkHttpClient.create(target.baseUrl()).build()) {
            HttpResponse<T> response = http.get(target.path())
                .timeout(REQUEST_TIMEOUT)
                .submit(replyType)
                .toCompletableFuture()
                .join();
            return requireSuccess(response, url);
        }
    }

    private static <T> T requireSuccess(HttpResponse<T> response, String url) {
        if (response.status() < 200 || response.status() >= 300) {
            throw new IllegalStateException("HTTP " + response.status() + " from " + url);
        }
        return response.body();
    }

    private static RequestTarget target(String url) {
        URI uri = URI.create(url);
        String baseUrl = uri.getScheme() + "://" + uri.getRawAuthority();
        String path = uri.getRawPath();
        if (uri.getRawQuery() != null) {
            path += "?" + uri.getRawQuery();
        }
        return new RequestTarget(baseUrl, path);
    }

    private record RequestTarget(String baseUrl, String path) { }
}
