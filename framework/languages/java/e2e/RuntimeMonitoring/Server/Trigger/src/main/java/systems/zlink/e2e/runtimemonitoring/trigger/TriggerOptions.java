package systems.zlink.e2e.runtimemonitoring.trigger;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("e2e")
public record TriggerOptions(
    String apiEndpoint, String serviceBApiEndpoint, String triggerHttpEndpoint, String logDirectory) {
    public TriggerOptions {
        required(apiEndpoint, "api-endpoint"); serviceBApiEndpoint = serviceBApiEndpoint == null ? "" : serviceBApiEndpoint;
        required(triggerHttpEndpoint, "trigger-http-endpoint"); required(logDirectory, "log-directory");
    }
    private static void required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException("e2e." + name + " is required");
    }
}
