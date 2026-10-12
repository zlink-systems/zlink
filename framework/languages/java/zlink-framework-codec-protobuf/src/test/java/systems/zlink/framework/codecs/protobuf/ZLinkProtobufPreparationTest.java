package systems.zlink.framework.codecs.protobuf;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.google.protobuf.AbstractParser;
import com.google.protobuf.ByteString;
import com.google.protobuf.CodedInputStream;
import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.ExtensionRegistryLite;
import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;
import com.google.protobuf.StringValue;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.io.OutputStream;

final class ZLinkProtobufPreparationTest {
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
    }

    public static final class CountingMessage implements MessageLite {
        static int parserSelections;
        private final StringValue value;

        CountingMessage(StringValue value) {
            this.value = value;
        }

        public static Parser<CountingMessage> parser() {
            parserSelections++;
            return new AbstractParser<>() {
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
