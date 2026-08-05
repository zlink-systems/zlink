plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}

kotlin {
    jvmToolchain(22)
}
