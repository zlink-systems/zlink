package systems.zlink.framework.runtime.internal.service;

import java.util.List;

/** Immutable, validated service record owned by the JVM Framework runtime. */
public record ZLinkServiceWireFrame(int command, int flags, List<byte[]> frames) {
    public ZLinkServiceWireFrame {
        frames = frames.stream().map(byte[]::clone).toList();
    }

    @Override
    public List<byte[]> frames() {
        return frames.stream().map(byte[]::clone).toList();
    }
}
