package systems.zlink.framework.streams;

public interface ZLinkSessionClient {
    ZLinkSessionSendCall send(Object message);

    ZLinkSessionReplyCall reply(Object message);
}
