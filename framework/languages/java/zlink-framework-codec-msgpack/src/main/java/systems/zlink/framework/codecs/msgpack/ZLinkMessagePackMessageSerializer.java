package systems.zlink.framework.codecs.msgpack;

import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;

final class ZLinkMessagePackMessageSerializer implements ZLinkMessageSerializer {
    static final ZLinkMessagePackMessageSerializer INSTANCE = new ZLinkMessagePackMessageSerializer();

    private ZLinkMessagePackMessageSerializer() {
    }

    @Override
    public <T> ZLinkEncodedPayload serialize(T value) {
        return ZLinkEncodedPayload.from(ZLinkMessagePackPayloads.encode(value, "payload"));
    }

    @Override
    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
        return ZLinkMessagePackPayloads.decode(payload.bytes(), type, "payload");
    }
}
