package systems.zlink.framework.locations;

public record ZLinkPageRequest(int pageSize, String continuationToken) {

    public static ZLinkPageRequest firstPage() {
        return new ZLinkPageRequest(100, null);
    }
}
