package systems.zlink.framework.perf;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializationFeature;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import com.fasterxml.jackson.datatype.jdk8.Jdk8Module;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;

import systems.zlink.contracts.core.RoutingId;

import java.io.IOException;

// The mapper of the admin surface. It writes the public Framework status records as they are (component names, enum
// names, Optional and Instant/Duration values) so an original preserves the language's own status shape (§15.4).
public final class ServerJson {
    private ServerJson() {}

    private static final ObjectMapper MAPPER = create();

    private static ObjectMapper create() {
        SimpleModule routingIds = new SimpleModule("perf-routing-id");
        routingIds.addSerializer(RoutingId.class, new JsonSerializer<>() {
            @Override
            public void serialize(RoutingId value, JsonGenerator generator, SerializerProvider serializers)
                    throws IOException {
                generator.writeString(value.toHex());
            }
        });
        return JsonMapper.builder()
                .addModule(new Jdk8Module())
                .addModule(new JavaTimeModule())
                .addModule(routingIds)
                .configure(SerializationFeature.WRITE_DATES_AS_TIMESTAMPS, false)
                .configure(SerializationFeature.WRITE_DURATIONS_AS_TIMESTAMPS, false)
                .configure(SerializationFeature.FAIL_ON_EMPTY_BEANS, false)
                .build();
    }

    public static ObjectMapper mapper() {
        return MAPPER;
    }

    public static String write(Object value) {
        try {
            return MAPPER.writeValueAsString(value);
        } catch (IOException error) {
            throw new IllegalStateException("Admin response is not serializable: " + error.getMessage(), error);
        }
    }
}
