package systems.zlink.httpclient;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.concurrent.CompletionException;

final class HttpOneShotR3Test {
    private record Response(int id) {}

    @Test
    void oneShotTypedSuccessAndSingleSubmission() throws Exception {
        var server =
                TestSupport.httpServer(
                        exchange -> TestSupport.respond(exchange, 200, "{\"id\":7}"));
        try {
            var request = ZLinkHttpClient.create(server.baseUrl()).get("/java-r3-success");
            assertEquals(
                    7, request.submit(Response.class).toCompletableFuture().join().body().id());
            assertThrows(ZLinkFrameworkException.class, request::submitRaw);
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void oneShotTypedStatusFailureAndSingleSubmission() throws Exception {
        var server = TestSupport.httpServer(exchange -> TestSupport.respond(exchange, 500, "{}"));
        try {
            var request = ZLinkHttpClient.create(server.baseUrl()).get("/java-r3-status");
            var error =
                    assertThrows(
                            CompletionException.class,
                            () -> request.submit(Response.class).toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    ((ZLinkFrameworkException) error.getCause()).kind());
            assertThrows(ZLinkFrameworkException.class, request::submitRaw);
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void oneShotTypedDecodeFailureAndSingleSubmission() throws Exception {
        var server =
                TestSupport.httpServer(
                        exchange -> TestSupport.respond(exchange, 200, "invalid-json"));
        try {
            var request = ZLinkHttpClient.create(server.baseUrl()).get("/java-r3-decode");
            var error =
                    assertThrows(
                            CompletionException.class,
                            () -> request.submit(Response.class).toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    ((ZLinkFrameworkException) error.getCause()).kind());
            assertThrows(ZLinkFrameworkException.class, request::submitRaw);
        } finally {
            server.closeable().close();
        }
    }
}
