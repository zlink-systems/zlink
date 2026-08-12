package systems.zlink.e2e.registrationcodec.codecrequester.Configuration;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record CodecRequesterOptions(
    String serverEndpoint,
    String httpEndpoint,
    String logDir) {
    public CodecRequesterOptions {
        required(serverEndpoint, "server-endpoint"); required(httpEndpoint, "http-endpoint"); required(logDir, "log-dir");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
