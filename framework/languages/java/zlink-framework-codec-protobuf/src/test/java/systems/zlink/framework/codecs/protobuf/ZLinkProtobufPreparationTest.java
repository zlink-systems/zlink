package systems.zlink.framework.codecs.protobuf;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.google.protobuf.AbstractParser;
import com.google.protobuf.ByteString;
import com.google.protobuf.BytesValue;
import com.google.protobuf.CodedInputStream;
import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.ExtensionRegistryLite;
import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;
import com.google.protobuf.StringValue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;

final class ZLinkProtobufPreparationTest {
    @Test
    void rawStreamMessagesOwnStorageAcrossEncodingAndDecoding() {
        var codec = ZLinkProtobufCodec.defaultCodec();
        byte[] expected = {0, -1, 67};
        ZLinkStreamEncodedPayload encoded;
        try (Message input = Message.from(expected)) {
            encoded = codec.encode("raw", input);
            input.fill((byte) 0);
            assertTrue(encoded.payload().contentEquals(expected));
        }
        Message decoded;
        try (Message frame = encoded.payload()) {
            decoded = codec.decode(encoded, Message.class);
            frame.fill((byte) 127);
            assertTrue(decoded.contentEquals(expected));
        }
        try (decoded) {
            assertTrue(decoded.contentEquals(expected));
        }
    }

    @Test
    void streamFallbackPreservesBytesTextAndJsonValues() {
        var codec = ZLinkProtobufCodec.defaultCodec();
        byte[] input = {0, -1, 67};
        var encodedBytes = codec.encode("bytes", input);
        try (Message frame = encodedBytes.payload()) {
            input[0] = 99;
            byte[] decoded = codec.decode(encodedBytes, byte[].class);
            frame.fill((byte) 0);
            assertArrayEquals(new byte[] {0, -1, 67}, decoded);
        }
        var encodedText = codec.encode("text", "한글😀");
        try (Message frame = encodedText.payload()) {
            assertEquals("한글😀", codec.decode(encodedText, String.class));
        }
        var expected = new FallbackValue("json:42");
        var encodedJson = codec.encode("json", expected);
        try (Message frame = encodedJson.payload()) {
            assertEquals(expected, codec.decode(encodedJson, FallbackValue.class));
        }
    }

    record FallbackValue(String id) {}

    @Test
    void borrowedProtobufBytesDoNotAliasTheNativeInput() {
        var original = BytesValue.of(ByteString.copyFromUtf8("owned-value".repeat(1024)));
        var serializer = ZLinkProtobufMessageSerializer.INSTANCE;
        BytesValue decoded;
        try (var payload = serializer.serializeOwned(original)) {
            decoded = serializer.deserializeBorrowed(payload, BytesValue.class);
            payload.fill((byte) 0);
            assertEquals(original, decoded);
        }
        assertEquals(original, decoded);
    }

    @Test
    void ownedCodecWritesIntoNativeStorageAndParsesItsView() throws Exception {
        CountingMessage.byteArrayEncodes = 0;
        var serializer = ZLinkProtobufMessageSerializer.INSTANCE;
        var original = StringValue.of("owned-message");
        StringValue decoded;
        try (var payload = serializer.serializeOwned(new CountingMessage(original))) {
            assertEquals(0, CountingMessage.byteArrayEncodes);
            assertEquals(original.getSerializedSize(), payload.size());
            decoded = serializer.deserializeBorrowed(payload, StringValue.class);
        }
        assertEquals(original, decoded);
    }

    @Test
    void registeredParserIsPreparedOnceAndReusedForEveryDecode() {
        var serializer = ZLinkProtobufMessageSerializer.INSTANCE;
        CountingMessage.parserSelections = 0;
        serializer.prepare(CountingMessage.class);
        assertEquals(1, CountingMessage.parserSelections);
        var payload = serializer.serialize(new CountingMessage(StringValue.of("message")));
        for (int index = 0; index < 3; index++) {
            assertEquals(
                    "message",
                    serializer.deserialize(payload, CountingMessage.class).value.getValue());
        }
        assertEquals(1, CountingMessage.parserSelections);

        CountingMessage.byteArrayEncodes = 0;
        CountingMessage.arrayParses = 0;
        var codec = ZLinkProtobufCodec.defaultCodec();
        var encoded = codec.encode("message", new CountingMessage(StringValue.of("stream-value")));
        CountingMessage decoded;
        try (var streamPayload = encoded.payload()) {
            decoded = codec.decode(encoded, CountingMessage.class);
        }
        assertEquals("stream-value", decoded.value.getValue());
        assertAll(
                () -> assertEquals(0, CountingMessage.byteArrayEncodes, "stream array encodes"),
                () -> assertEquals(0, CountingMessage.arrayParses, "stream heap parse inputs"));
    }

    public static final class CountingMessage implements MessageLite {
        static int parserSelections;
        static int byteArrayEncodes;
        static int arrayParses;
        private final StringValue value;

        CountingMessage(StringValue value) {
            this.value = value;
        }

        public static Parser<CountingMessage> parser() {
            parserSelections++;
            return new AbstractParser<>() {
                @Override
                public CountingMessage parseFrom(ByteBuffer payload)
                        throws InvalidProtocolBufferException {
                    if (payload.hasArray()) arrayParses++;
                    return super.parseFrom(payload);
                }

                @Override
                public CountingMessage parsePartialFrom(
                        CodedInputStream input, ExtensionRegistryLite extensions)
                        throws InvalidProtocolBufferException {
                    return new CountingMessage(
                            StringValue.parser().parsePartialFrom(input, extensions));
                }
            };
        }

        @Override
        public Parser<CountingMessage> getParserForType() {
            return parser();
        }

        @Override
        public void writeTo(CodedOutputStream output) throws IOException {
            value.writeTo(output);
        }

        @Override
        public int getSerializedSize() {
            return value.getSerializedSize();
        }

        @Override
        public ByteString toByteString() {
            return value.toByteString();
        }

        @Override
        public byte[] toByteArray() {
            byteArrayEncodes++;
            return value.toByteArray();
        }

        @Override
        public void writeTo(OutputStream output) throws IOException {
            value.writeTo(output);
        }

        @Override
        public void writeDelimitedTo(OutputStream output) throws IOException {
            value.writeDelimitedTo(output);
        }

        @Override
        public Builder newBuilderForType() {
            return value.newBuilderForType();
        }

        @Override
        public Builder toBuilder() {
            return value.toBuilder();
        }

        @Override
        public MessageLite getDefaultInstanceForType() {
            return new CountingMessage(StringValue.getDefaultInstance());
        }

        @Override
        public boolean isInitialized() {
            return value.isInitialized();
        }
    }
}
