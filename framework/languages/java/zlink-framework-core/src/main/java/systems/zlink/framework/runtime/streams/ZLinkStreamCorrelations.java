package systems.zlink.framework.runtime.streams;

final class ZLinkStreamCorrelations {
    private ZLinkStreamCorrelations() { }

    static String forTrace(ZLinkStreamHeader header) {
        return header.correlationId().orElse(null);
    }
}
