package systems.zlink.framework.configuration;

public interface ZLinkDispatchOptions {
    ZLinkUnhandledDispatchOptions unhandled();

    ZLinkDiagnosticsOptions diagnostics();

    ZLinkDispatchOptions messageFlow(ZLinkMessageFlowLogMode mode);

    ZLinkDispatchOptions traceSampleRate(double rate);

    ZLinkDispatchOptions includeMessageSizes(boolean include);
}
