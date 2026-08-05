package systems.zlink.framework.docexamples.stream;

/** 가이드 9장이 쓰는 최소 packet 계약. */
public final class StreamMessages {
    private StreamMessages() {
    }

    public record Ping(long sequence) {
    }

    public record Pong(long sequence) {
    }

    public record ServerNotice(String reason) {
    }
}
