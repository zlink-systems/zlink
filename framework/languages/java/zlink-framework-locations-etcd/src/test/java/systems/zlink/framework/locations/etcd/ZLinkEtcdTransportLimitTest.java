package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.locations.etcd.ZLinkEtcdProviderTest.KEY;
import static systems.zlink.framework.locations.etcd.ZLinkEtcdProviderTest.NEVER;
import static systems.zlink.framework.locations.etcd.ZLinkEtcdProviderTest.assertFailure;
import static systems.zlink.framework.locations.etcd.ZLinkEtcdProviderTest.await;

import com.google.protobuf.ByteString;
import com.google.protobuf.UnknownFieldSet;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.AuthenticateRequest;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.AuthenticateResponse;

import java.io.IOException;

/** Spec 07 §9.2 client bounds, checked at the exact serialized message size. */
class ZLinkEtcdTransportLimitTest {
    @Test
    void sendLimitAcceptsExactly16MiBAndFailsOneByteMoreBeforeDispatch() throws Exception {
        for (int excess : new int[] {0, 1}) {
            String password = passwordForRequestSize(EtcdClient.MAX_SEND_MESSAGE_BYTES + excess);
            try (var backend = new EtcdTestServer();
                    var store = new ZLinkEtcdLocationStore(withCredentials(backend, password))) {
                if (excess == 0) {
                    assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
                    assertEquals(1, backend.authCalls.get());
                    assertEquals(1, backend.txnCalls.get());
                } else {
                    assertFailure(IOException.class, () -> await(store.read(KEY, NEVER)));
                    assertEquals(0, backend.authCalls.get());
                    assertEquals(0, backend.txnCalls.get());
                }
            }
        }
    }

    @Test
    void receiveLimitAcceptsExactly8MiBAndFailsOneByteMore() throws Exception {
        for (int excess : new int[] {0, 1}) {
            int size = EtcdClient.MAX_RECEIVE_MESSAGE_BYTES + excess;
            try (var backend = new EtcdTestServer();
                    var store = new ZLinkEtcdLocationStore(withCredentials(backend, "p"))) {
                backend.authReply = response -> padded(response, size);
                if (excess == 0) {
                    assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
                    assertEquals(1, backend.txnCalls.get());
                } else {
                    assertFailure(IOException.class, () -> await(store.read(KEY, NEVER)));
                    assertEquals(0, backend.txnCalls.get());
                }
                assertEquals(1, backend.authCalls.get());
            }
        }
    }

    static ZLinkEtcdLocationOptions withCredentials(EtcdTestServer backend, String password) {
        return backend.options().setCredentials(new ZLinkEtcdCredentials("u", password));
    }

    static AuthenticateRequest authentication(String password) {
        return AuthenticateRequest.newBuilder().setName("u").setPassword(password).build();
    }

    // Both password lengths need a four-byte varint, so the field overhead is the same.
    static String passwordForRequestSize(int size) {
        int probe = authentication("p".repeat(size)).getSerializedSize();
        String password = "p".repeat(2 * size - probe);
        assertEquals(size, authentication(password).getSerializedSize());
        return password;
    }

    static AuthenticateResponse padded(AuthenticateResponse response, int size) {
        int overhead = withUnknownField(response, size).getSerializedSize() - size;
        AuthenticateResponse padded = withUnknownField(response, size - overhead);
        assertEquals(size, padded.getSerializedSize());
        return padded;
    }

    // Field 1000 is unknown to AuthenticateResponse: it adds bytes the client skips after its
    // size check, so the token stays short.
    static AuthenticateResponse withUnknownField(AuthenticateResponse response, int length) {
        ByteString padding = ByteString.copyFrom(new byte[length]);
        var field = UnknownFieldSet.Field.newBuilder().addLengthDelimited(padding).build();
        var builder = response.toBuilder();
        builder.setUnknownFields(UnknownFieldSet.newBuilder().addField(1000, field).build());
        return builder.build();
    }
}
