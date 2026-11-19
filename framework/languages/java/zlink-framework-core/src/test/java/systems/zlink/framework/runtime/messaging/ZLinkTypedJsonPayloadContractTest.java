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
import java.util.List;
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

    record ListPayload(@Nullable List<Child> items, Child @Nullable [] array) {}

    record ChildMapPayload(Map<String, Child> value) {}

    record DepthPayload(@Nullable Object a, @Nullable Object b) {}

    record ArraysPayload(
            String @Nullable [] names,
            int @Nullable [] nums,
            long @Nullable [] longs,
            boolean @Nullable [] flags,
            @Nullable Child child) {}

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

    private static void rejectsDuplicate(String json, Class<?> type) {
        var error =
                assertThrows(
                        JsonProcessingException.class,
                        () -> ZLinkFrameworkJsonProfile.mapper().readValue(json, type),
                        json);
        assertTrue(error.getOriginalMessage().startsWith("Duplicate field"), error.getMessage());
    }

    @Test
    void duplicatesInsideArrayAndMapValueDtosAreRejected() {
        rejectsDuplicate("{\"items\":[{\"text\":\"a\",\"text\":\"b\"}]}", ListPayload.class);
        rejectsDuplicate(
                "{\"items\":[{\"text\":\"a\"},{\"text\":\"b\",\"text\":\"c\"}]}",
                ListPayload.class);
        rejectsDuplicate("{\"array\":[{\"text\":\"a\",\"text\":\"b\"}]}", ListPayload.class);
        rejectsDuplicate(
                "{\"value\":{\"k\":{\"text\":\"a\",\"text\":\"b\"}}}", ChildMapPayload.class);
    }

    @Test
    void duplicateScopeReturnsToTheOuterObject() throws Exception {
        var mapper = ZLinkFrameworkJsonProfile.mapper();
        var accepted = mapper.readValue("{\"a\":{\"a\":1},\"b\":2}", DepthPayload.class);
        assertEquals(Map.of("a", 1), accepted.a());
        assertEquals(2, accepted.b());
        var siblings =
                mapper.readValue(
                        "{\"items\":[{\"text\":\"a\"},{\"text\":\"b\"}],"
                                + "\"array\":[{\"text\":\"c\"}]}",
                        ListPayload.class);
        assertEquals(2, siblings.items().size());
        rejectsDuplicate("{\"a\":{\"x\":1},\"a\":2}", DepthPayload.class);
        rejectsDuplicate("{\"a\":{\"x\":{\"y\":1}},\"b\":1,\"b\":2}", DepthPayload.class);
    }

    @Test
    void duplicateCreatorPropertiesAreRejectedWhenJacksonBuffers() {
        rejectsDuplicate(
                "{\"wireValue\":{\"text\":\"a\"},\"wireValue\":{\"text\":\"b\"}}",
                ObjectPayload.class);
        rejectsDuplicate(
                "{\"extra\":{\"k\":1},\"wireValue\":{\"text\":\"a\"},"
                        + "\"wireValue\":{\"text\":\"b\"}}",
                ObjectPayload.class);
        rejectsDuplicate(
                "{\"extra\":1,\"wireValue\":{\"text\":\"a\"},\"extra\":2}", ObjectPayload.class);
    }

    @Test
    void primitiveArrayReadersKeepDuplicateDepth() throws Exception {
        String arrays = "\"names\":[\"a\"],\"nums\":[1],\"longs\":[2],\"flags\":[true]";
        rejectsDuplicate(
                "{" + arrays + ",\"child\":{\"text\":\"x\",\"text\":\"y\"}}", ArraysPayload.class);
        rejectsDuplicate("{" + arrays + ",\"names\":[\"b\"]}", ArraysPayload.class);
        var accepted =
                ZLinkFrameworkJsonProfile.mapper()
                        .readValue(
                                "{" + arrays + ",\"child\":{\"names\":1,\"text\":\"x\"}}",
                                ArraysPayload.class);
        assertEquals("x", accepted.child().text());
        assertEquals(1, accepted.nums()[0]);
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
