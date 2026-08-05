package systems.zlink.e2e.pubsub.client.Support;

import java.net.URI;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

final class PubSubHttpClient implements AutoCloseable {
    private final Map<String, ZLinkHttpClient> clients = new LinkedHashMap<>();

    String get(String url) {
        RequestTarget target = target(url);
        RawHttpResponse response = target.client().get(target.path())
            .submitRaw()
            .toCompletableFuture()
            .join();
        return requireSuccess(response, url);
    }

    String post(String url) {
        RequestTarget target = target(url);
        RawHttpResponse response = target.client().post(target.path())
            .submitRaw()
            .toCompletableFuture()
            .join();
        return requireSuccess(response, url);
    }

    boolean isHealthy(String endpoint) {
        try {
            get(endpoint + "/health");
            return true;
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    boolean isHealthy(String endpoint, Duration timeout) {
        try {
            RequestTarget target = target(endpoint + "/health");
            RawHttpResponse response = target.client().get(target.path())
                .timeout(timeout)
                .submitRaw()
                .toCompletableFuture()
                .join();
            return response.status() >= 200 && response.status() < 300;
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    private RequestTarget target(String url) {
        URI uri = URI.create(url);
        String baseUrl = uri.getScheme() + "://" + uri.getRawAuthority();
        String path = uri.getRawPath();
        if (uri.getRawQuery() != null) {
            path += "?" + uri.getRawQuery();
        }
        ZLinkHttpClient client = clients.computeIfAbsent(
            baseUrl,
            endpoint -> ZLinkHttpClient.create(endpoint).build());
        return new RequestTarget(client, path);
    }

    private static String requireSuccess(RawHttpResponse response, String url) {
        if (response.status() < 200 || response.status() >= 300) {
            throw new IllegalStateException(
                "HTTP " + response.status() + " from " + url + ": " + response.body());
        }
        return response.body();
    }

    @Override
    public void close() {
        for (ZLinkHttpClient client : clients.values()) {
            client.close();
        }
        clients.clear();
    }

    private record RequestTarget(ZLinkHttpClient client, String path) { }
}
