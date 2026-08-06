package systems.zlink.e2e.pubsub.client.Support;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.httpclient.RawHttpResponse;

public final class ScenarioContext implements AutoCloseable {
    private final ClientOptions options;
    private final PublisherClient publisher;
    private final Evidence evidence;
    private final ServerProcessLauncher processes;
    private final PubSubHttpClient http;
    private final ObjectMapper json;

    private ScenarioContext(ClientOptions options, PubSubHttpClient http) {
        this.options = options;
        this.json = new ObjectMapper();
        this.publisher = new PublisherClient(http, options.publisherHttp());
        this.evidence = new Evidence(options, json, http);
        this.processes = new ServerProcessLauncher(options, http);
        this.http = http;
    }

    public static ScenarioContext load(String[] args) {
        ClientOptions options = ClientOptions.load(args);
        return new ScenarioContext(options, new PubSubHttpClient());
    }

    public ClientOptions options() {
        return options;
    }

    public PublisherClient publisher() {
        return publisher;
    }

    public Evidence evidence() {
        return evidence;
    }

    public ServerProcessLauncher processes() {
        return processes;
    }

    public PublisherClient publisherAt(String endpoint) {
        return new PublisherClient(http, endpoint);
    }

    public String get(String url) { return http.get(url); }
    public String post(String url) { return http.post(url); }
    public RawHttpResponse postRaw(String url) { return http.postRaw(url); }
    public boolean isHealthy(String endpoint) { return http.isHealthy(endpoint); }
    public ObjectMapper json() { return json; }

    @Override
    public void close() {
        http.close();
    }
}
