package MyGame;

public final class HttpProtocol {
    private HttpProtocol() {}

    public static final String POST = "POST",
            CONTENT_TYPE = "Content-Type",
            JSON = "application/json";
    public static final int OK = 200, BAD_REQUEST = 400, NOT_FOUND = 404, UNAVAILABLE = 503;
}
