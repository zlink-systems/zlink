package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkFrameworkRuntimeCodecTest {
    @Test
    void predicateCodecExtensionKeepsJsonFallbackForOtherPayloadTypes() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.codecs().use(MarkerCodecExtension::register);

        ZLinkMessageSerializer serializer = ZLinkFrameworkRuntime.serializerFor(options);

        Marker marker = serializer.deserialize(serializer.serialize(new Marker("custom")), Marker.class);
        assertEquals(new Marker("custom"), marker);

        Fallback fallback = serializer.deserialize(serializer.serialize(new Fallback("json")), Fallback.class);
        assertEquals(new Fallback("json"), fallback);
    }

    record Marker(String value) {
    }

    record Fallback(String value) {
    }

    static final class MarkerSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            Marker marker = (Marker) value;
            return ZLinkEncodedPayload.from(("MARKER:" + marker.value()).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            String text = new String(payload.bytes(), StandardCharsets.UTF_8);
            String value = text.startsWith("MARKER:") ? text.substring("MARKER:".length()) : text;
            return type.cast(new Marker(value));
        }
    }

    static final class MarkerCodecExtension {
        static void register(ZLinkCodecRegistrar codecs) {
            codecs.addSerializer("application/x-marker", new MarkerSerializer(), Marker.class::equals);
        }
    }
}
