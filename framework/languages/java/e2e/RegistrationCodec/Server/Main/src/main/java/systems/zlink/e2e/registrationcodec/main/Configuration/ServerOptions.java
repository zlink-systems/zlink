package systems.zlink.e2e.registrationcodec.main.Configuration;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ServerOptions(
    String serverEndpoint,
    String httpEndpoint,
    String logDir) {
    public ServerOptions {
        required(serverEndpoint, "server-endpoint"); required(httpEndpoint, "http-endpoint"); required(logDir, "log-dir");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
