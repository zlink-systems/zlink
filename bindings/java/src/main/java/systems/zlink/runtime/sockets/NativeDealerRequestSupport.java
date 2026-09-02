/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.util.List;

final class NativeDealerRequestSupport {
    static {
        InternalAccess.register(new InternalAccess.RuntimeSocketAccess() {
            @Override
            public Object routerReceiveSupport(RouterSocket socket,
                                               boolean closeSocketOnClose) {
                return new NativeRouterReceiveSupport(socket,
                    closeSocketOnClose);
            }

            @Override
            public Received routerRecv(Object support, RecvFlags flags) {
                return ((NativeRouterReceiveSupport) support).recv(flags);
            }

            @Override
            public boolean routerRecvInto(Object support, Received target,
                                          RecvFlags flags) {
                return ((NativeRouterReceiveSupport) support).recvInto(target,
                    flags);
            }

            @Override
            public void routerReceiveBeginClose(Object support) {
                ((NativeRouterReceiveSupport) support).beginClose();
            }

            @Override
            public void routerReceiveFinishClose(Object support) {
                ((NativeRouterReceiveSupport) support).finishClose();
            }

            @Override
            public void routerReply(RouterSocket socket, RoutingId routingId,
                                    long replyTokenValue, List<Message> parts) {
                if (!(socket instanceof NativeRouterSocket nativeSocket)) {
                    throw new IllegalArgumentException(
                        "router is not backed by the native runtime");
                }
                nativeSocket.submitReply(routingId, replyTokenValue, parts);
            }

        });
    }

    private NativeDealerRequestSupport() {
    }

}
