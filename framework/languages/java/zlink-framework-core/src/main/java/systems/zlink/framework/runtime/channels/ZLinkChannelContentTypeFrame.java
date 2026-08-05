package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.messaging.Message;

/** Carries the framework-selected application content type inside internal frames. */
public final class ZLinkChannelContentTypeFrame {
    public static final String DEFAULT_CONTENT_TYPE = "application/json";
    private static final String PREFIX = "__zlink.content-type\n";

    private ZLinkChannelContentTypeFrame() {
    }

    public static Message encode(String contentType) {
        String value = Objects.requireNonNull(contentType, "contentType").trim();
        if (value.isEmpty()) {
            throw new IllegalArgumentException("content type must not be blank");
        }
        return Message.from((PREFIX + value).getBytes(StandardCharsets.UTF_8));
    }

    public static String decode(List<Message> parts) {
        if (parts == null) {
            return null;
        }
        for (int index = 2; index < parts.size(); index++) {
            String value = parts.get(index).toUtf8String();
            if (value.startsWith(PREFIX)) {
                String contentType = value.substring(PREFIX.length()).trim();
                return contentType.isEmpty() ? null : contentType;
            }
        }
        return null;
    }
}
