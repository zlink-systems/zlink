package systems.zlink.framework.codecs.protobuf;

import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;

import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

final class ZLinkProtobufMessageSerializer implements ZLinkMessageSerializer {
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

    static boolean canSerialize(Class<?> type) {
        return type != null && MessageLite.class.isAssignableFrom(type);
    }

    @Override
    public <T> ZLinkEncodedPayload serialize(T value) {
        if (value instanceof MessageLite protobuf) {
            return ZLinkEncodedPayload.from(protobuf.toByteArray());
        }
        throw new IllegalArgumentException(
                "Protobuf codec cannot serialize value of type "
                        + ZLinkProtobufPayloads.valueTypeName(value));
    }

    @Override
    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
        if (canSerialize(type)) {
            return parseProtobuf(payload, type);
        }
        throw new IllegalArgumentException(
                "Protobuf codec cannot deserialize payload as " + type.getName());
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

    private <T> T parseProtobuf(ZLinkEncodedPayload payload, Class<T> type) {
        try {
            Parser<?> parser = parsers.get(type);
            return type.cast(parser.parseFrom(payload.bytes()));
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
