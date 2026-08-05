package systems.zlink.framework.channels;

public interface ZLinkSocketRuntimeOptions {
    long maxMessageSize();

    void maxMessageSize(long value);

    int weight();

    void weight(int value);
}
