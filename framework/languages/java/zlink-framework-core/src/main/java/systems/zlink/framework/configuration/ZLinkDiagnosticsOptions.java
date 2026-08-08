package systems.zlink.framework.configuration;

public interface ZLinkDiagnosticsOptions {
    ZLinkMessageFlowLogMode messageFlow();

    double sampleRate();

    boolean includeMessageSizes();
}
