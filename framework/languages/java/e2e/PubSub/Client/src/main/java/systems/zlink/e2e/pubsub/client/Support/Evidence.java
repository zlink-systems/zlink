package systems.zlink.e2e.pubsub.client.Support;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class Evidence {
    private final ClientOptions options;
    private final ObjectMapper json;
    private final PubSubHttpClient http;

    Evidence(ClientOptions options, ObjectMapper json, PubSubHttpClient http) {
        this.options = options;
        this.json = json;
        this.http = http;
    }

    public Contracts.EvidenceSnapshot snapshot(String subscriberRid) {
        String endpoint = options.subscriberHttp(subscriberRid);
        try {
            String body = http.get(endpoint + "/evidence");
            return json.readValue(body, Contracts.EvidenceSnapshot.class);
        } catch (Exception error) {
            throw new IllegalStateException("failed to fetch evidence from " + subscriberRid, error);
        }
    }

    public Contracts.EvidenceSnapshot waitForAnyEvent(String subscriberRid, String scenario) {
        return waitFor(subscriberRid, "kind=any-event&scenario=" + encode(scenario));
    }

    public Contracts.EvidenceSnapshot waitForEvent(
        String subscriberRid,
        String scenario,
        int sequence) {
        return waitFor(
            subscriberRid,
            "kind=event&scenario=" + encode(scenario) + "&sequence=" + sequence);
    }

    public Contracts.EvidenceSnapshot waitForSequenceAtLeast(
        String subscriberRid,
        String scenario,
        int minSequence) {
        return waitFor(
            subscriberRid,
            "kind=sequence-at-least&scenario=" + encode(scenario) + "&minSequence=" + minSequence);
    }

    public Contracts.EvidenceSnapshot waitForDispatchError(
        String subscriberRid,
        String packetName) {
        return waitFor(
            subscriberRid,
            "kind=dispatch-error&packetName=" + encode(packetName));
    }

    public Contracts.EvidenceSnapshot waitForContiguousRun(
        String subscriberRid,
        String scenario,
        int minLength) {
        return waitFor(
            subscriberRid,
            "kind=contiguous-run&scenario=" + encode(scenario) + "&minLength=" + minLength);
    }

    private Contracts.EvidenceSnapshot waitFor(String subscriberRid, String query) {
        String endpoint = options.subscriberHttp(subscriberRid);
        try {
            String body = http.get(endpoint + "/evidence/wait?" + query);
            return json.readValue(body, Contracts.EvidenceSnapshot.class);
        } catch (Exception error) {
            throw new IllegalStateException("failed to wait for evidence from " + subscriberRid, error);
        }
    }

    private static String encode(String value) {
        return URLEncoder.encode(value, StandardCharsets.UTF_8);
    }
}
