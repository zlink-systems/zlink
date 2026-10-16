package systems.zlink.framework.codecs.protobuf;

import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

final class ZLinkProtobufMessageSerializer
        implements ZLinkMessageSerializer, ZLinkOwnedMessageSerializer {
    private static final String PARSER_METHOD_NAME = "parser";
    static final ZLinkProtobufMessageSerializer INSTANCE = new ZLinkProtobufMessageSerializer();
    private final ClassValue<Parser<?>> parsers =
            new ClassValue<>() {
                @Override
                protected Parser<?> computeValue(Class<?> type) {
                    return parserFor(type);
                }
            };

    private ZLinkProtobufMessageSerializer() {}

    @Override
    public Message serializeOwned(Object value) {
        MessageLite protobuf = requireProtobuf(value);
        Message encoded = Message.allocate(protobuf.getSerializedSize());
        boolean transferred = false;
        try {
            CodedOutputStream output = CodedOutputStream.newInstance(encoded.mutableDataBuffer());
            protobuf.writeTo(output);
            output.flush();
            output.checkNoSpaceLeft();
            transferred = true;
            return encoded;
        } catch (IOException failure) {
            throw new IllegalArgumentException("failed to serialize Protobuf message", failure);
        } finally {
            if (!transferred) encoded.close();
        }
    }

    private static MessageLite requireProtobuf(Object value) {
        if (value instanceof MessageLite protobuf) {
            return protobuf;
        }
        throw new IllegalArgumentException(
                "Protobuf codec cannot serialize value of type "
                        + ZLinkProtobufPayloads.valueTypeName(value));
    }

    static boolean canSerialize(Class<?> type) {
        return type != null && MessageLite.class.isAssignableFrom(type);
    }

    @Override
    public <T> ZLinkEncodedPayload serialize(T value) {
        return ZLinkEncodedPayload.from(requireProtobuf(value).toByteArray());
    }

    @Override
    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
        return parseProtobuf(ByteBuffer.wrap(payload.bytes()), type);
    }

    @Override
    public <T> T deserializeBorrowed(Message message, Class<T> type) {
        return parseProtobuf(message.dataBuffer(), type);
    }

    @Override
    public void prepare(Class<?> type) {
        if (type != null && !canSerialize(type)) {
            throw new IllegalArgumentException(
                    "Protobuf codec cannot prepare payload type " + type.getName());
        }
        if (type != null) {
            parsers.get(type);
        }
    }

    private <T> T parseProtobuf(ByteBuffer payload, Class<T> type) {
        if (!canSerialize(type)) {
            throw new IllegalArgumentException(
                    "Protobuf codec cannot deserialize payload as " + type.getName());
        }
        try {
            Parser<?> parser = parsers.get(type);
            return type.cast(parser.parseFrom(payload));
        } catch (InvalidProtocolBufferException ex) {
            throw new IllegalArgumentException(
                    "failed to deserialize Protobuf message as " + type.getName(), ex);
        }
    }

    private static Parser<?> parserFor(Class<?> type) {
        try {
            Method parserMethod = type.getMethod(PARSER_METHOD_NAME);
            Object parser = parserMethod.invoke(null);
            if (parser instanceof Parser<?> typedParser) {
                return typedParser;
            }
            throw new IllegalArgumentException(
                    "Protobuf type parser() did not return Parser: " + type.getName());
        } catch (NoSuchMethodException ex) {
            throw new IllegalArgumentException(
                    "Protobuf type does not expose parser(): " + type.getName(), ex);
        } catch (IllegalAccessException ex) {
            throw new IllegalArgumentException(
                    "Protobuf parser() is not accessible: " + type.getName(), ex);
        } catch (InvocationTargetException ex) {
            throw new IllegalArgumentException(
                    "Protobuf parser() failed for " + type.getName(), ex.getCause());
        }
    }
}
