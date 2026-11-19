package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.core.JsonProcessingException;

import org.jspecify.annotations.Nullable;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.stream.Stream;

final class ZLinkTypedJsonPayloadContractTest {
    record ConstructorPayload(byte[] body) {}

    public static final class PropertyPayload {
        public byte[] body = new byte[0];
    }

    static Stream<Arguments> rows() throws Exception {
        Path root = Path.of("").toAbsolutePath();
        while (!Files.exists(root.resolve("framework/test/fixtures/typed-json-payload.tsv")))
            root = root.getParent();
        return Files.readAllLines(root.resolve("framework/test/fixtures/typed-json-payload.tsv"))
                .stream()
                .filter(line -> !line.startsWith("#") && !line.isEmpty())
                .map(
                        line -> {
                            String[] fields = line.split("\t", -1);
                            return Arguments.of(
                                    fields[0], fields[1].equals("accept"), fields[2], fields[3]);
                        });
    }

    record StringPayload(@JsonProperty("wireValue") String value) {}

    record Child(String text) {}

    record ObjectPayload(@JsonProperty("wireValue") Child value) {}

    record MapPayload(Map<String, Object> value) {}

    record NullableReferences(@Nullable String text, @Nullable Child child) {}

    record NullablePayload(byte @Nullable [] body) {}

    @Test
    void explicitlyNullableBytesAreAccepted() throws Exception {
        assertNull(
                ZLinkFrameworkJsonProfile.mapper()
                        .readValue("{\"body\":null}", NullablePayload.class)
                        .body());
    }

    @Test
    void duplicatePropertiesAreRejectedInTypedAndDynamicObjects() {
        var mapper = ZLinkFrameworkJsonProfile.mapper();
        assertThrows(
                JsonProcessingException.class,
                () ->
                        mapper.readValue(
                                "{\"wireValue\":{\"text\":\"first\",\"text\":\"second\"}}",
                                ObjectPayload.class));
        assertThrows(
                JsonProcessingException.class,
                () ->
                        mapper.readValue(
                                "{\"wireValue\":{\"text\":\"one\",\"text\":\"two\",\"text\":\"three\"}}",
                                ObjectPayload.class));
        assertThrows(
                JsonProcessingException.class,
                () ->
                        mapper.readValue(
                                "{\"wireValue\":{\"text\":\"value\",\"unknown\":1,\"unknown\":2}}",
                                ObjectPayload.class));
        assertThrows(
                JsonProcessingException.class,
                () -> mapper.readValue("{\"value\":{\"key\":1,\"key\":2}}", MapPayload.class));
    }

    @Test
    void propertyNamesRemainCaseSensitiveForDuplicateDetection() throws Exception {
        var result =
                ZLinkFrameworkJsonProfile.mapper()
                        .readValue(
                                "{\"wireValue\":{\"text\":\"value\",\"Text\":\"ignored\"}}",
                                ObjectPayload.class);
        assertEquals("value", result.value().text());
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("rows")
    void sharedTypedContract(String id, boolean accept, String shape, String json) {
        if (shape.startsWith("encode-")) {
            Object value =
                    switch (shape) {
                        case "encode-string" -> new StringPayload(null);
                        case "encode-object" -> new ObjectPayload(null);
                        case "encode-nullable" -> new NullableReferences(null, null);
                        default -> throw new AssertionError(shape);
                    };
            String wire = null;
            Exception failure = null;
            try {
                // This is the same mapper used by the framework codec and JVM connectors.
                wire = ZLinkFrameworkJsonProfile.mapper().writeValueAsString(value);
            } catch (Exception error) {
                failure = error;
            }
            System.out.println("ENCODE_CONTRACT " + id + " wire=" + wire + " error=" + failure);
            assertEquals(accept, failure == null, id);
            if (accept) assertEquals(json, wire);
            else
                assertTrue(
                        failure.getMessage().contains("Property 'wireValue' does not allow null."),
                        failure.toString());
            ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
            if (accept)
                assertEquals(
                        json,
                        new String(serializer.serialize(value).bytes(), StandardCharsets.UTF_8));
            else {
                IllegalArgumentException error =
                        assertThrows(
                                IllegalArgumentException.class, () -> serializer.serialize(value));
                assertTrue(
                        error.getCause()
                                .getMessage()
                                .contains("Property 'wireValue' does not allow null."));
            }
            return;
        }
        boolean accepted;
        try {
            Class<?> type = shape.equals("ctor") ? ConstructorPayload.class : PropertyPayload.class;
            ZLinkFrameworkJsonProfile.mapper()
                    .readValue(json.getBytes(StandardCharsets.UTF_8), type);
            accepted = true;
        } catch (Exception error) {
            accepted = false;
        }
        assertEquals(accept, accepted, id);
    }

    static final class ConstructorDeclaration {
        @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
        @java.lang.annotation.Target(java.lang.annotation.ElementType.PARAMETER)
        @interface Nullable {}
    }

    record DeclarationNullable(@ConstructorDeclaration.Nullable String value) {}

    @Test
    void encodeUsesDecodeDeclarationIncludingConstructorOnlyAnnotations() throws Exception {
        var mapper = ZLinkFrameworkJsonProfile.mapper();
        assertNull(mapper.readValue("{\"value\":null}", DeclarationNullable.class).value());
        assertEquals("{\"value\":null}", mapper.writeValueAsString(new DeclarationNullable(null)));
        assertEquals("{\"wireValue\":\"ok\"}", mapper.writeValueAsString(new StringPayload("ok")));
        assertEquals(
                "{\"wireValue\":{\"text\":\"ok\"}}",
                mapper.writeValueAsString(new ObjectPayload(new Child("ok"))));
        var nested =
                assertThrows(
                        com.fasterxml.jackson.databind.JsonMappingException.class,
                        () -> mapper.writeValueAsString(new ObjectPayload(new Child(null))));
        assertTrue(nested.getMessage().contains("Property 'text' does not allow null."));
        PropertyPayload fields = new PropertyPayload();
        fields.body = null;
        var field =
                assertThrows(
                        com.fasterxml.jackson.databind.JsonMappingException.class,
                        () -> mapper.writeValueAsString(fields));
        assertTrue(field.getMessage().contains("Property 'body' does not allow null."));
    }
}
