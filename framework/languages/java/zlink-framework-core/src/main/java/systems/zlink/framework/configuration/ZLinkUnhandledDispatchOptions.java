package systems.zlink.framework.configuration;

public interface ZLinkUnhandledDispatchOptions {
    void setRequest(ZLinkUnhandledDispatchAction action);

    void setSend(ZLinkUnhandledDispatchAction action);

    void setPublish(ZLinkUnhandledDispatchAction action);

    void setSendLogLevel(ZLinkLogLevel level);

    void setPublishLogLevel(ZLinkLogLevel level);
}
