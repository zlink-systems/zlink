package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.io.IOException;
import java.nio.charset.Charset;
import java.util.List;

final class ZLinkJsonInputEncodingTest {
    record Value(String value) {}

    @Test
    void heapAndNativeInputsPreserveSynchronousProfileValuesAndErrors() throws Exception {
        ObjectMapper previous = ZLinkFrameworkJsonProfile.mapper();
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        for (String encoding : List.of("UTF-8", "UTF-16BE", "UTF-16LE", "UTF-32BE", "UTF-32LE")) {
            for (boolean bom : new boolean[] {false, true}) {
                for (String json :
                        List.of(
                                "{\"value\":\"한글😀\"}",
                                "{",
                                "{}",
                                "{\"value\":\"first\",\"value\":\"second\"}")) {
                    byte[] bytes =
                            ((bom ? "\uFEFF" : "") + json).getBytes(Charset.forName(encoding));
                    String context = encoding + " BOM=" + bom + " JSON=" + json;
                    Value expected;
                    try {
                        expected = previous.readValue(bytes, Value.class);
                    } catch (IOException error) {
                        IllegalArgumentException heap =
                                assertThrows(
                                        IllegalArgumentException.class,
                                        () ->
                                                serializer.deserialize(
                                                        ZLinkEncodedPayload.from(bytes),
                                                        Value.class),
                                        context);
                        assertEquals(error.getClass(), heap.getCause().getClass(), context);
                        assertEquals(error.getMessage(), heap.getCause().getMessage(), context);
                        try (Message input = Message.from(bytes)) {
                            IllegalArgumentException nativeError =
                                    assertThrows(
                                            IllegalArgumentException.class,
                                            () ->
                                                    serializer.deserializeBorrowed(
                                                            input.dataBuffer(), Value.class),
                                            context);
                            assertEquals(
                                    error.getClass(), nativeError.getCause().getClass(), context);
                            assertEquals(
                                    error.getMessage(),
                                    nativeError.getCause().getMessage(),
                                    context);
                        }
                        continue;
                    }
                    assertEquals(new Value("한글😀"), expected, context);
                    assertEquals(
                            expected,
                            serializer.deserialize(ZLinkEncodedPayload.from(bytes), Value.class),
                            context);
                    try (Message input = Message.from(bytes)) {
                        assertEquals(
                                expected,
                                serializer.deserializeBorrowed(input.dataBuffer(), Value.class),
                                context);
                    }
                }
            }
        }
    }
}
