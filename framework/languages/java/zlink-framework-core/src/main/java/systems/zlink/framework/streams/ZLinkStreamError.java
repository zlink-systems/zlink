package systems.zlink.framework.streams;

public record ZLinkStreamError(
    ZLinkStreamSessionError error,
    String message) {
}
