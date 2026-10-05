package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

interface ZLinkJavaSocketBacked extends ZLinkBackendObject {
    Socket nativeSocket();
}
