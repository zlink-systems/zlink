package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkCodecSelectionConformanceTest {
    private static final ZLinkMessageSerializer JSON = new TaggedSerializer("json");

    @Test
    void consumesRegistrationNormalizationScenarios() throws Exception {
        JsonNode fixture = fixture();
        assertEquals("zlink.framework.codec-selection", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());

        for (JsonNode scenario : fixture.path("normalizationScenarios")) {
            ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
            String input = scenario.path("input").asText();
            if (scenario.has("expectedError")) {
                assertEquals("configurationError", scenario.path("expectedError").asText());
                assertThrows(
                    ZLinkConfigurationException.class,
                    () -> registration.addSerializer(input, new TaggedSerializer("candidate")));
                continue;
            }
            registration.addSerializer(input, new TaggedSerializer("candidate"));
            assertEquals(
                List.of(scenario.path("expected").asText()),
                List.copyOf(registration.serializers().keySet()),
                scenario.path("name").asText());
        }
    }

    @Test
    void consumesNormalizedDuplicateReplacementScenario() throws Exception {
        JsonNode scenario = fixture().path("normalizedDuplicateScenario");
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        List<ZLinkMessageSerializer> registrations = new ArrayList<>();

        for (JsonNode input : scenario.path("registrationInputs")) {
            ZLinkMessageSerializer serializer =
                new TaggedSerializer("registration-" + registrations.size());
            registrations.add(serializer);
            registration.addSerializer(input.asText(), serializer, ignored -> true);
        }

        assertEquals(scenario.path("finalEntryCount").asInt(), registration.serializers().size());
        int selected = scenario.path("selectedRegistrationIndex").asInt();
        assertSame(
            registrations.get(selected),
            registration.serializerForSending(BaseMessage.class, JSON));
        assertEquals("application/x-base", registration.contentTypeFor(BaseMessage.class));
    }

    @Test
    void consumesDeclaredTypeSendSelectionScenarios() throws Exception {
        JsonNode fixture = fixture();
        Map<String, JsonNode> extensions = extensionsByName(fixture);

        for (JsonNode scenario : fixture.path("sendScenarios")) {
            ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
            Map<String, TaggedSerializer> serializers = new HashMap<>();
            for (JsonNode registrationName : scenario.path("registrationOrder")) {
                String name = registrationName.asText();
                JsonNode extension = extensions.get(name);
                TaggedSerializer serializer = new TaggedSerializer(name);
                serializers.put(name, serializer);
                registration.addSerializer(
                    extension.path("contentType").asText(),
                    serializer,
                    declared -> matchesDeclaredType(extension, declared));
                registration.addStreamCodec(
                    extension.path("contentType").asText(),
                    streamCodec(name));
            }
            registration.freeze();

            Class<?> declaredType = messageType(scenario.path("declaredType").asText());
            Object runtimeValue = messageValue(scenario.path("runtimeType").asText());
            String expectedCodec = scenario.path("expectedCodec").asText();
            ZLinkMessageSerializer expectedSerializer = "json".equals(expectedCodec)
                ? JSON
                : serializers.get(expectedCodec);

            assertEquals(
                scenario.path("expectedContentType").asText(),
                registration.contentTypeFor(declaredType),
                scenario.path("name").asText());
            assertSame(
                expectedSerializer,
                registration.serializerForSending(declaredType, JSON),
                scenario.path("name").asText());

            ZLinkMessageSerializer composite = registration.serializerWithFallback(JSON);
            ZLinkEncodedPayload declaredEncoded =
                ZLinkCodecRegistration.serializeForDeclaredType(
                    composite, runtimeValue, declaredType);
            assertTrue(
                text(declaredEncoded).startsWith(expectedCodec + ":"),
                scenario.path("name").asText());

            ZLinkMessage outbound = ZLinkMessage.of(runtimeValue, declaredType);
            assertEquals(
                "json".equals(expectedCodec)
                    ? ZLinkStreamCodec.JSON
                    : streamCodec(expectedCodec),
                ZLinkPayloadEncoding.streamCodec(
                    composite, outbound, ZLinkStreamCodec.RAW),
                scenario.path("name").asText());
            ZLinkPayloadEncoding.EncodedPayload wireEncoded =
                ZLinkPayloadEncoding.encode(composite, outbound);
            try {
                assertEquals(
                    scenario.path("expectedContentType").asText(),
                    wireEncoded.contentType(),
                    scenario.path("name").asText());
                assertEquals(
                    declaredType.getSimpleName(),
                    wireEncoded.packetName(),
                    scenario.path("name").asText());
                assertTrue(
                    new String(
                        wireEncoded.payload().toByteArray(),
                        StandardCharsets.UTF_8).startsWith(expectedCodec + ":"),
                    scenario.path("name").asText());
            } finally {
                wireEncoded.payload().close();
            }
        }
    }

    @Test
    void consumesExactReceiveSelectionScenarios() throws Exception {
        JsonNode fixture = fixture();
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        Map<String, TaggedSerializer> serializers = new HashMap<>();
        for (JsonNode extension : fixture.path("extensions")) {
            String name = extension.path("name").asText();
            TaggedSerializer serializer = new TaggedSerializer(name);
            serializers.put(name, serializer);
            registration.addSerializer(
                extension.path("contentType").asText(),
                serializer,
                declared -> matchesDeclaredType(extension, declared));
        }
        registration.freeze();

        for (JsonNode scenario : fixture.path("receiveScenarios")) {
            String wireContentType = scenario.path("wireContentType").asText();
            if ("success".equals(scenario.path("expectedTerminal").asText())) {
                assertSame(
                    serializers.get(scenario.path("expectedCodec").asText()),
                    registration.serializerForReceivedContentType(wireContentType, JSON),
                    scenario.path("name").asText());
                continue;
            }
            assertEquals("protocolError", scenario.path("expectedTerminal").asText());
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> registration.serializerForReceivedContentType(wireContentType, JSON),
                scenario.path("name").asText());
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
    }

    @Test
    void startupSnapshotRejectsLateRegistration() throws Exception {
        JsonNode invariants = fixture().path("registryInvariants");
        assertTrue(invariants.path("immutableAfterStartup").asBoolean());
        assertTrue(invariants.path("receiveTableBuiltAtStartup").asBoolean());

        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        TaggedSerializer startup = new TaggedSerializer("startup");
        registration.addSerializer("application/x-startup", startup, ignored -> true);
        registration.freeze();

        assertSame(
            startup,
            registration.serializerForReceivedContentType("application/x-startup", JSON));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> registration.addSerializer(
                "application/x-late", new TaggedSerializer("late"), ignored -> true));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> registration.use(codecs -> codecs.addSerializer(
                "application/x-late", new TaggedSerializer("late"))));
    }

    @Test
    void selectedCustomStreamCodecRequiresAnExactMapping() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addSerializer(
            "application/x-unmapped",
            new TaggedSerializer("unmapped"),
            BaseMessage.class::equals);
        ZLinkMessageSerializer composite = registration.serializerWithFallback(JSON);

        ZLinkConfigurationException failure = assertThrows(
            ZLinkConfigurationException.class,
            () -> ZLinkPayloadEncoding.streamCodec(
                composite,
                ZLinkMessage.of(new BaseMessage(), BaseMessage.class),
                ZLinkStreamCodec.JSON));

        assertTrue(failure.getMessage().contains("does not have a STREAM codec mapping"));
    }

    @Test
    void consumesBoundedNoEvictionCacheScenario() throws Exception {
        JsonNode scenario = fixture().path("cacheScenarios").get(0);
        int distinctTypes = scenario.path("distinctDeclaredTypes").asInt();
        int cachedTypes = scenario.path("cachedTypes").asInt();
        assertEquals(
            fixture().path("limits").path("sendTypeCacheCapacity").asInt(),
            cachedTypes);
        assertEquals(distinctTypes - cachedTypes, scenario.path("uncachedTypes").asInt());
        assertFalse(scenario.path("existingCachedSelectionChanges").asBoolean());
        assertTrue(scenario.path("uncachedTypeReevaluatesSelector").asBoolean());

        AtomicInteger selectorCalls = new AtomicInteger();
        AtomicBoolean matches = new AtomicBoolean(true);
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addSerializer(
            "application/x-cache",
            new TaggedSerializer("cache"),
            ignored -> {
                selectorCalls.incrementAndGet();
                return matches.get();
            });
        registration.freeze();

        List<Class<?>> types = distinctProxyTypes(distinctTypes);
        for (Class<?> type : types) {
            assertEquals("application/x-cache", registration.contentTypeFor(type));
        }
        assertEquals(distinctTypes, selectorCalls.get());

        matches.set(false);
        assertEquals("application/x-cache", registration.contentTypeFor(types.get(0)));
        assertEquals(distinctTypes, selectorCalls.get());
        assertEquals("application/json", registration.contentTypeFor(types.get(types.size() - 1)));
        assertEquals(distinctTypes + 1, selectorCalls.get());
    }

    @Test
    void concurrentFirstUseEvaluatesSelectorOnce() throws Exception {
        AtomicInteger selectorCalls = new AtomicInteger();
        CountDownLatch selectorEntered = new CountDownLatch(1);
        CountDownLatch releaseSelector = new CountDownLatch(1);
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addSerializer(
            "application/x-single-flight",
            new TaggedSerializer("single-flight"),
            ignored -> {
                selectorCalls.incrementAndGet();
                selectorEntered.countDown();
                try {
                    if (!releaseSelector.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("selector release timed out");
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
                return true;
            });
        registration.freeze();

        var executor = Executors.newFixedThreadPool(12);
        try {
            CountDownLatch start = new CountDownLatch(1);
            List<Future<String>> results = new ArrayList<>();
            for (int index = 0; index < 24; index++) {
                results.add(executor.submit(() -> {
                    start.await();
                    return registration.contentTypeFor(BaseMessage.class);
                }));
            }
            start.countDown();
            assertTrue(selectorEntered.await(5, TimeUnit.SECONDS));
            releaseSelector.countDown();
            for (Future<String> result : results) {
                assertEquals("application/x-single-flight", result.get(5, TimeUnit.SECONDS));
            }
            assertEquals(1, selectorCalls.get());
        } finally {
            releaseSelector.countDown();
            executor.shutdownNow();
        }
    }

    private static Map<String, JsonNode> extensionsByName(JsonNode fixture) {
        Map<String, JsonNode> extensions = new HashMap<>();
        for (JsonNode extension : fixture.path("extensions")) {
            extensions.put(extension.path("name").asText(), extension);
        }
        return extensions;
    }

    private static boolean matchesDeclaredType(JsonNode extension, Class<?> declaredType) {
        for (JsonNode matched : extension.path("matchesDeclaredTypes")) {
            if (matched.asText().equals(declaredType.getSimpleName())) {
                return true;
            }
        }
        return false;
    }

    private static Class<?> messageType(String name) {
        return switch (name) {
            case "BaseMessage" -> BaseMessage.class;
            case "DerivedMessage" -> DerivedMessage.class;
            case "OtherMessage" -> OtherMessage.class;
            case "UnregisteredMessage" -> UnregisteredMessage.class;
            default -> throw new AssertionError("unknown fixture message type: " + name);
        };
    }

    private static Object messageValue(String name) {
        return switch (name) {
            case "BaseMessage" -> new BaseMessage();
            case "DerivedMessage" -> new DerivedMessage();
            case "OtherMessage" -> new OtherMessage();
            case "UnregisteredMessage" -> new UnregisteredMessage();
            default -> throw new AssertionError("unknown fixture runtime type: " + name);
        };
    }

    private static ZLinkStreamCodec streamCodec(String extensionName) {
        return switch (extensionName) {
            case "baseCodec" -> ZLinkStreamCodec.PROTOBUF;
            case "broadCodec" -> ZLinkStreamCodec.MESSAGE_PACK;
            case "otherCodec" -> ZLinkStreamCodec.RAW;
            default -> throw new AssertionError(
                "unknown fixture codec extension: " + extensionName);
        };
    }

    @SuppressWarnings("deprecation")
    private static List<Class<?>> distinctProxyTypes(int count) {
        List<Class<?>> types = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            ClassLoader loader = new ClassLoader(
                ZLinkCodecSelectionConformanceTest.class.getClassLoader()) {
            };
            types.add(Proxy.getProxyClass(loader, Runnable.class));
        }
        return types;
    }

    private static String text(ZLinkEncodedPayload payload) {
        return new String(payload.bytes(), StandardCharsets.UTF_8);
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/conformance/codec-selection-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared codec selection fixture was not found");
    }

    static class BaseMessage {
    }

    static final class DerivedMessage extends BaseMessage {
    }

    static final class OtherMessage {
    }

    static final class UnregisteredMessage {
    }

    private record TaggedSerializer(String tag) implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            String type = value == null ? "null" : value.getClass().getSimpleName();
            return ZLinkEncodedPayload.from(
                (tag + ":" + type).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException("selection tests do not decode payloads");
        }
    }
}
