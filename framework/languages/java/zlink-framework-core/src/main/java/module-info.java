module systems.zlink.framework {
    requires transitive systems.zlink;
    requires transitive zlink.framework.provider.abstractions;
    requires zlink.framework.binding.internal;
    requires com.fasterxml.jackson.databind;
    requires static org.jspecify;
    requires org.lz4.java;
    requires org.slf4j;
    requires java.management;
    requires jdk.management;
    requires java.logging;

    exports systems.zlink.framework;
    exports systems.zlink.framework.actors;
    exports systems.zlink.framework.channels;
    exports systems.zlink.framework.configuration;
    exports systems.zlink.framework.errors;
    exports systems.zlink.framework.execution;
    exports systems.zlink.framework.handlers;
    exports systems.zlink.framework.locations;
    exports systems.zlink.framework.messaging;
    exports systems.zlink.framework.monitoring;
    exports systems.zlink.framework.spots;
    exports systems.zlink.framework.streams;
    exports systems.zlink.framework.runtime.host;

    // Companion artifacts use these implementation contracts. They are not
    // exported to application modules.
    exports systems.zlink.framework.runtime.internal.backend to
        zlink.framework.spring.boot.starter,
        zlink.framework.testkit;
    exports systems.zlink.framework.runtime.internal.diagnostics to
        zlink.framework.kotlin;
    exports systems.zlink.framework.runtime.internal.handlers to
        zlink.framework.spring.boot.starter,
        zlink.framework.kotlin;
    exports systems.zlink.framework.runtime.internal.host to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.internal.metrics to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.internal.monitoring to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.binding to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.actors to
        zlink.framework.testkit;
    exports systems.zlink.framework.runtime.configuration to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.handlers to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.locations to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.mesh to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.spots to
        zlink.framework.spring.boot.starter;
    exports systems.zlink.framework.runtime.streams to
        zlink.framework.spring.boot.starter,
        zlink.framework.testkit;

    uses systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
    uses systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
}
