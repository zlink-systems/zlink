module zlink.framework.testkit {
    requires transitive systems.zlink.framework;
    requires zlink.framework.binding.internal;
    requires transitive zlink.stream.connector;

    exports systems.zlink.framework.testkit;
}
