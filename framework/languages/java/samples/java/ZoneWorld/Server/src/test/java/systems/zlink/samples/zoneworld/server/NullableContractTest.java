package systems.zlink.samples.zoneworld.server;

import static org.junit.jupiter.api.Assertions.assertNull;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.samples.zoneworld.shared.Messages.EnterWorldRes;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

final class NullableContractTest {
    @Test
    void typedJsonAcceptsNullForOptionalResponseError() {
        var serializer = new ZLinkJsonMessageSerializer();
        var response =
                serializer.deserializeBorrowed(
                        ByteBuffer.wrap(
                                "{\"zoneId\":\"zone-1\",\"x\":1,\"y\":2,\"error\":null}"
                                        .getBytes(StandardCharsets.UTF_8)),
                        EnterWorldRes.class);

        assertNull(response.error());
    }
}
