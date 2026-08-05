package systems.zlink.e2e.pubsub.client.Support;

import com.fasterxml.jackson.databind.ObjectMapper;

public final class ScenarioContext implements AutoCloseable {
    private final ClientOptions options;
    private final PublisherClient publisher;
    private final Evidence evidence;
    private final ServerProcessLauncher processes;
    private final PubSubHttpClient http;

    private ScenarioContext(ClientOptions options, PubSubHttpClient http) {
        this.options = options;
        this.publisher = new PublisherClient(http, options.publisherHttp());
        this.evidence = new Evidence(options, new ObjectMapper(), http);
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

    @Override
    public void close() {
        http.close();
    }
}
