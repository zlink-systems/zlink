package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import com.google.protobuf.ByteString;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.*;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;

/** Runs against a dedicated mTLS and password-authenticated etcd, without a Store clock. */
class ZLinkEtcdClientLiveTest {
    static String required(String name) {
        String value = System.getenv(name);
        assumeTrue(value != null && !value.isEmpty(), "Set " + name + " for secure etcd tests");
        return value;
    }

    static ZLinkEtcdLocationOptions options() throws Exception {
        return new ZLinkEtcdLocationOptions()
                .setNamespace("client-capability")
                .setEndpoints(List.of(required("ZLINK_ETCD_CLIENT_ENDPOINT")))
                .setCredentials(
                        new ZLinkEtcdCredentials(
                                required("ZLINK_ETCD_CLIENT_USER"),
                                required("ZLINK_ETCD_CLIENT_PASSWORD")))
                .setTls(
                        new ZLinkEtcdTlsOptions()
                                .setTrustedCaPem(
                                        Files.readString(Path.of(required("ZLINK_ETCD_CLIENT_CA"))))
                                .setClientIdentity(
                                        new ZLinkEtcdClientIdentity(
                                                Files.readString(
                                                        Path.of(
                                                                required(
                                                                        "ZLINK_ETCD_CLIENT_CERT"))),
                                                Files.readString(
                                                        Path.of(
                                                                required(
                                                                        "ZLINK_ETCD_CLIENT_KEY"))))));
    }

    static <T> T await(CompletableFuture<T> result) throws Exception {
        return result.get(10, TimeUnit.SECONDS);
    }

    @Test
    void authenticatedMutualTlsPreservesRevisionAndKeysOnlyOmitsValues() throws Exception {
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        var key = ByteString.copyFromUtf8("client-capability/" + UUID.randomUUID());
        var payload = ByteString.copyFrom(new byte[] {1, 0, -1});
        try (var client = new EtcdClient(options());
                var operation = new EtcdClient.Operation(client.timeoutNanos, null, scheduler)) {
            try {
                var put = PutRequest.newBuilder().setKey(key).setValue(payload);
                var written =
                        await(
                                client.txn(
                                        TxnRequest.newBuilder()
                                                .addSuccess(
                                                        RequestOp.newBuilder().setRequestPut(put))
                                                .build(),
                                        operation));
                long revision = written.getHeader().getRevision();
                await(
                        client.txn(
                                TxnRequest.newBuilder()
                                        .addSuccess(
                                                RequestOp.newBuilder()
                                                        .setRequestPut(
                                                                put.setValue(
                                                                        ByteString.copyFromUtf8(
                                                                                "replacement"))))
                                        .build(),
                                operation));
                var request = RangeRequest.newBuilder().setKey(key).setRevision(revision);
                var full = await(client.range(request.build(), operation));
                var keys = await(client.range(request.setKeysOnly(true).build(), operation));
                assertEquals(1, full.getKvsCount());
                assertEquals(payload, full.getKvs(0).getValue());
                assertEquals(revision, full.getKvs(0).getModRevision());
                assertEquals(1, keys.getKvsCount());
                assertTrue(keys.getKvs(0).getValue().isEmpty());
                assertEquals(full.getKvs(0).toBuilder().clearValue().build(), keys.getKvs(0));
                assertEquals(full.getHeader().getClusterId(), keys.getHeader().getClusterId());
            } finally {
                await(
                        client.txn(
                                TxnRequest.newBuilder()
                                        .addSuccess(
                                                RequestOp.newBuilder()
                                                        .setRequestDeleteRange(
                                                                DeleteRangeRequest.newBuilder()
                                                                        .setKey(key)))
                                        .build(),
                                operation));
            }
        } finally {
            scheduler.shutdownNow();
        }
    }

    static void rejects(ZLinkEtcdLocationOptions options) throws Exception {
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        try (var client = new EtcdClient(options);
                var operation = new EtcdClient.Operation(client.timeoutNanos, null, scheduler)) {
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    await(
                                            client.range(
                                                    RangeRequest.newBuilder()
                                                            .setKey(
                                                                    ByteString.copyFromUtf8(
                                                                            "client-capability/missing"))
                                                            .build(),
                                                    operation)));
            Throwable cause = failure;
            while (cause.getCause() != null) cause = cause.getCause();
            assertInstanceOf(IOException.class, cause);
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void wrongPasswordIsStoreFailure() throws Exception {
        var options = options();
        options.setCredentials(
                new ZLinkEtcdCredentials(
                        options.credentials().user(), "wrong-" + UUID.randomUUID()));
        rejects(options);
    }

    @Test
    void untrustedCaIsStoreFailure() throws Exception {
        var options = options();
        options.setTls(new ZLinkEtcdTlsOptions().setClientIdentity(options.tls().clientIdentity()));
        rejects(options);
    }

    @Test
    void wrongHostnameIsStoreFailure() throws Exception {
        var options = options();
        assertTrue(
                options.endpoints().get(0).contains("localhost"),
                "Use localhost with a DNS-only test certificate");
        options.setEndpoints(List.of(options.endpoints().get(0).replace("localhost", "127.0.0.1")));
        rejects(options);
    }

    @Test
    void missingClientIdentityIsStoreFailure() throws Exception {
        var options = options();
        options.setTls(new ZLinkEtcdTlsOptions().setTrustedCaPem(options.tls().trustedCaPem()));
        rejects(options);
    }
}
