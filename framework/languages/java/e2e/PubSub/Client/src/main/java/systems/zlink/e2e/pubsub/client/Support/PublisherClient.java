package systems.zlink.e2e.pubsub.client.Support;

import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class PublisherClient {
    private final PubSubHttpClient http;
    private final String endpoint;

    PublisherClient(PubSubHttpClient http, String endpoint) {
        this.http = http;
        this.endpoint = endpoint;
    }

    public void publish(String topic, Contracts.EventMsg message) {
        post("/publish/event", topic, message);
    }

    public void publishMissing(String topic, Contracts.EventMsg message) {
        post("/publish/missing", topic, message);
    }

    public boolean canReachHealth() {
        return http.isHealthy(endpoint);
    }

    private void post(String path, String topic, Contracts.EventMsg message) {
        String uri = endpoint + path
            + "?topic=" + encode(topic)
            + "&scenario=" + encode(message.scenario())
            + "&sequence=" + message.sequence()
            + "&value=" + encode(message.value());
        try {
            http.post(uri);
        } catch (Exception error) {
            throw new IllegalStateException("failed to publish through " + endpoint + path, error);
        }
    }

    private static String encode(String value) {
        return URLEncoder.encode(value, StandardCharsets.UTF_8);
    }
}
