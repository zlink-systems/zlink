package systems.zlink.e2e.registrationcodec.invalidduplicate.Configuration;
import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record ServerOptions(String serverEndpoint) {
    public ServerOptions {
        if (serverEndpoint == null || serverEndpoint.isBlank()) {
            throw new IllegalArgumentException("e2e.server-endpoint is required");
        }
    }
}
