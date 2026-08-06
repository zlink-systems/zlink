package systems.zlink.e2e.instancespot.client;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.core.env.StandardEnvironment;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0])
            || args[1].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: instance-spot-client --config <path>");
        }
        SpringApplicationBuilder builder = new SpringApplicationBuilder(
            ClientApplication.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location="
                + Path.of(args[1]).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        builder.run();
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(
            StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(
            StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}

