package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.util.JsonParserDelegate;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.exc.InvalidFormatException;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.List;
import java.util.Map;

final class ZLinkJsonBytesTest {
    private static final String ALPHABET =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    record BytesValue(byte[] body) {}

    @ParameterizedTest
    @ValueSource(ints = {0, 1, 2, 3, 127, 128, 1024, 4096})
    void preservesExactJsonAndOwnedBytes(int size) {
        byte[] body = bytes(size);
        byte[] expected = json(Base64.getEncoder().encodeToString(body));
        var serializer = new ZLinkJsonMessageSerializer();
        var value = new BytesValue(body);
        ZLinkEncodedPayload encoded = serializer.serialize(value);
        assertArrayEquals(expected, encoded.bytes());
        BytesValue decoded = serializer.deserialize(encoded, BytesValue.class);
        BytesValue second = serializer.deserialize(encoded, BytesValue.class);
        assertArrayEquals(body, decoded.body());
        assertArrayEquals(body, second.body());
        assertNotSame(body, decoded.body());
        assertNotSame(decoded.body(), second.body());
        try (Message message = serializer.serializeOwned(value)) {
            assertArrayEquals(expected, message.toByteArray());
            BytesValue borrowed =
                    serializer.deserializeBorrowed(message.dataBuffer(), BytesValue.class);
            assertArrayEquals(body, borrowed.body());
            assertNotSame(body, borrowed.body());
        }
    }

    @ParameterizedTest
    @ValueSource(ints = {0, 1, 2, 3, 127, 128, 1024, 4096})
    void decodesCanonicalBytesWithoutMaterializingTokenStrings(int size) throws Exception {
        var mapper = ZLinkFrameworkJsonProfile.mapper();
        byte[] body = bytes(size);
        try (JsonParser parser =
                new JsonParserDelegate(
                        mapper.createParser(json(Base64.getEncoder().encodeToString(body)))) {
                    @Override
                    public String getText() throws IOException {
                        if (currentToken() == JsonToken.VALUE_STRING) {
                            throw new AssertionError(
                                    "bytes decoding must borrow the parser character range");
                        }
                        return super.getText();
                    }
                }) {
            assertArrayEquals(body, mapper.readValue(parser, BytesValue.class).body());
        }
    }

    @Test
    void readsNonzeroCharacterOffsetsAndEscapedBase64() throws Exception {
        ObjectMapper mapper = ZLinkFrameworkJsonProfile.mapper();
        char[] input = "  {\"body\":\"AQID\"}  ".toCharArray();
        int[] offset = {-1};
        try (JsonParser parser =
                new JsonParserDelegate(mapper.createParser(input)) {
                    @Override
                    public char[] getTextCharacters() throws IOException {
                        char[] characters = super.getTextCharacters();
                        if (currentToken() == JsonToken.VALUE_STRING) offset[0] = getTextOffset();
                        return characters;
                    }
                }) {
            assertArrayEquals(
                    new byte[] {1, 2, 3}, mapper.readValue(parser, BytesValue.class).body());
        }
        assertTrue(
                offset[0] > 0, "the parser must expose an offset inside its shared input buffer");
        assertArrayEquals(
                Base64.getDecoder().decode("/+8="),
                mapper.readValue("{\"body\":\"\\u002f\\u002b8=\"}", BytesValue.class).body());
    }

    @Test
    void rejectsEveryNonzeroPaddingBit() throws Exception {
        ObjectMapper mapper = ZLinkFrameworkJsonProfile.mapper();
        for (int digit = 0; digit < ALPHABET.length(); digit++) {
            String twoPadding = "A" + ALPHABET.charAt(digit) + "==";
            String onePadding = "AA" + ALPHABET.charAt(digit) + "=";
            checkPadding(mapper, twoPadding, (digit & 15) == 0);
            checkPadding(mapper, onePadding, (digit & 3) == 0);
        }
    }

    @Test
    void preservesInvalidBase64ErrorCategories() throws Exception {
        ObjectMapper mapper = ZLinkFrameworkJsonProfile.mapper();
        for (String value :
                List.of(
                        "A",
                        "AA",
                        "AAA",
                        "AAAA=",
                        "AA===",
                        "=AAA",
                        "A=AA",
                        "AA=A",
                        "====",
                        "AA==AAAA",
                        "AAAAAA==AAAA",
                        "AA==\n",
                        " AA=",
                        "AA =",
                        "AA-_'",
                        "AA-_",
                        "AAé=",
                        "AAĀ=",
                        "AA😀",
                        "AA\u0000=")) {
            JsonMappingException failure =
                    assertThrows(
                            JsonMappingException.class,
                            () ->
                                    mapper.readValue(
                                            mapper.writeValueAsBytes(Map.of("body", value)),
                                            BytesValue.class),
                            value);
            assertEquals(InvalidFormatException.class, failure.getClass(), value);
            String reason =
                    (value.length() & 3) == 0
                            ? "bytes are not canonical padded base64"
                            : "base64 padding is required";
            assertTrue(failure.getOriginalMessage().endsWith(": " + reason), value);
        }
        for (String value : List.of("0", "false", "[]", "{}")) {
            JsonMappingException failure =
                    assertThrows(
                            JsonMappingException.class,
                            () -> mapper.readValue("{\"body\":" + value + "}", BytesValue.class));
            assertEquals("bytes must be a padded base64 JSON string", failure.getOriginalMessage());
        }
    }

    private static void checkPadding(ObjectMapper mapper, String value, boolean canonical)
            throws IOException {
        if (canonical) {
            assertArrayEquals(
                    Base64.getDecoder().decode(value),
                    mapper.readValue(json(value), BytesValue.class).body());
        } else {
            JsonMappingException failure =
                    assertThrows(
                            JsonMappingException.class,
                            () -> mapper.readValue(json(value), BytesValue.class),
                            value);
            assertEquals(InvalidFormatException.class, failure.getClass(), value);
            assertTrue(
                    failure.getOriginalMessage()
                            .endsWith(": bytes are not canonical padded base64"),
                    value);
        }
    }

    private static byte[] json(String base64) {
        return ("{\"body\":\"" + base64 + "\"}").getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] bytes(int size) {
        byte[] body = new byte[size];
        for (int index = 0; index < size; index++) {
            body[index] = (byte) (31 * index + 17 * (index / 251) + 29);
        }
        return body;
    }
}
