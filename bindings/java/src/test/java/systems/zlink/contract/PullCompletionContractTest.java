/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.internal.ContractAccess;

final class PullCompletionContractTest {
    @Test
    void replyTokenEqualityIncludesOwnerIdentity() {
        Object firstOwner = new Object();
        Object secondOwner = new Object();
        ReplyToken first = ContractAccess.replyToken(firstOwner, 7L);
        ReplyToken same = ContractAccess.replyToken(firstOwner, 7L);
        ReplyToken otherOwner = ContractAccess.replyToken(secondOwner, 7L);
        assertEquals(first, same);
        assertEquals(first.hashCode(), same.hashCode());
        assertNotEquals(first, otherOwner);
        assertEquals("ReplyToken", first.toString());
    }

    @Test
    void ownerMismatchIsRejectedBeforeNativeReply() throws Exception {
        TestSupport.assumeNative();
        RoutingId dealerRid = RoutingId.from("reply-token-owner");
        try (Context context = Zlink.createContext();
             RouterSocket owner = context.createRouterSocket();
             RouterSocket other = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             Received request = new Received()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint("reply-token-owner");
            owner.bind(endpoint);
            dealer.connect(endpoint);
            var completion = dealer.request().message(Message.from("ping"))
                .timeout(Duration.ofSeconds(2)).submit();
            assertTrue(owner.recv(request, RecvFlags.NONE));
            ReplyToken token = request.replyToken().orElseThrow();
            assertThrows(IllegalArgumentException.class, () ->
                other.reply(dealerRid, token));
            request.reply().message(Message.from("pong")).submit();
            List<Message> reply = completion.toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            Message.closeAll(reply);
        }
    }

    @Test
    void streamPacketBeginResetsOwnedMessagesForReuse() {
        StreamPacket packet = new StreamPacket();
        Message header = Message.from("header");
        Message body = Message.from("body");
        ContractAccess.streamPacketBegin(packet);
        ContractAccess.streamPacketComplete(packet, RoutingId.from("peer"),
            header, body);
        assertEquals("body", packet.body().toUtf8String());
        ContractAccess.streamPacketBegin(packet);
        assertTrue(packet.isEmpty());
        assertEquals(0, body.size());
        ContractAccess.streamPacketFail(packet);
        packet.close();
    }
}
