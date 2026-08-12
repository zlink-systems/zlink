package systems.zlink.e2e.registrationcodec.client.Support;

import java.time.Duration;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class Evidence implements AutoCloseable {
    private final ZLinkHttpClient http;

    private Evidence(ZLinkHttpClient http) {
        this.http = http;
    }

    public static Evidence fromOptions(ClientOptions options) {
        return new Evidence(ZLinkHttpClient.create(options.httpEndpoint())
            .timeout(Duration.ofSeconds(3))
            .build());
    }

    public Contracts.EvidenceSnapshot snapshot() {
        return http.get("/evidence").submit(Contracts.EvidenceSnapshot.class).toCompletableFuture().join().body();
    }

    @Override
    public void close() {
        http.close();
    }
}
