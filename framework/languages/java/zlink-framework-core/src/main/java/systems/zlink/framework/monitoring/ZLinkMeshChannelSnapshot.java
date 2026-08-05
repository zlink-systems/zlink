package systems.zlink.framework.monitoring;

public record ZLinkMeshChannelSnapshot(
    String channelName,
    boolean isReady,
    int readyTargetCount) {
}
