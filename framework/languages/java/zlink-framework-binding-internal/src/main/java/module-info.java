module zlink.framework.binding.internal {
    requires systems.zlink;

    exports systems.zlink.framework.runtime.internal.binding.spot to
        systems.zlink.framework,
        zlink.framework.spring.boot.starter,
        zlink.framework.testkit;
}
