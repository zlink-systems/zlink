plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api(zlinkLibs.zlink.bindings)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("org.springframework:spring-context:6.2.18")
}
