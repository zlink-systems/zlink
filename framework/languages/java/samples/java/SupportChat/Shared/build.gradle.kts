plugins {
    `java-library`
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    runtimeOnly("org.jspecify:jspecify:1.0.0")
}
