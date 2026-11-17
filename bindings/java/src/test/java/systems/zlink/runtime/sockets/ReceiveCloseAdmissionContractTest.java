/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.StreamSocket;

class ReceiveCloseAdmissionContractTest {
    @Test
    void closeRejectsReceiveBeforeBorrowedRoutingIdIsCopied() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    @Test
    void closedStreamReceivePreservesNativeResult() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             StreamSocket socket = context.createStreamSocket();
             StreamPacket packet = new StreamPacket()) {
            socket.close();
            for (RecvFlags flags : new RecvFlags[] {RecvFlags.NONE, RecvFlags.DONT_WAIT}) {
                ZlinkRecvException failure = assertThrows(ZlinkRecvException.class,
                    () -> socket.recvPacket(packet, flags));
                assertEquals(RecvResult.INVALID_HANDLE, failure.getResult());
                assertEquals(systems.zlink.runtime.nativeapi.NativeErrno.EFAULT, failure.getNativeErrno());
            }
        }
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        core.blockStreamReceive = true;
        try (Context context = Zlink.createContext();
             StreamSocket socket = context.createStreamSocket();
             StreamPacket packet = new StreamPacket()) {
            FutureTask<Boolean> recv = new FutureTask<>(() -> socket.recvPacket(packet, RecvFlags.NONE));
            Thread thread = new Thread(recv, "receive-close");
            thread.start();
            try {
                assertTrue(core.streamReceiveReturned.await(5, TimeUnit.SECONDS));
                ZlinkCloseException failure = assertThrows(ZlinkCloseException.class, socket::close);
                assertEquals(CloseResult.BUSY, failure.getResult());
            } finally {
                core.releaseStreamReceive.countDown();
                assertTrue(recv.get(5, TimeUnit.SECONDS));
                thread.join();
            }
            assertArrayEquals(new byte[] {4, 5, 6}, packet.routingId().orElseThrow().toBytes());
            socket.close();
        }
        core.blockStreamReceive = false;
        for (RecvResult outcome : new RecvResult[] {RecvResult.NO_DATA, RecvResult.INTERNAL_ERROR}) {
            try (Context context = Zlink.createContext();
                 StreamSocket socket = context.createStreamSocket();
                 StreamPacket packet = new StreamPacket()) {
                core.receiveFault("zlink_stream_recv_packet", outcome,
                    systems.zlink.runtime.nativeapi.NativeErrno.EINTR);
                if (outcome == RecvResult.NO_DATA) {
                    assertFalse(socket.recvPacket(packet, RecvFlags.DONT_WAIT));
                } else {
                    ZlinkRecvException failure = assertThrows(ZlinkRecvException.class,
                        () -> socket.recvPacket(packet, RecvFlags.NONE));
                    assertEquals(outcome, failure.getResult());
                }
                socket.close();
            }
        }
    }
}
