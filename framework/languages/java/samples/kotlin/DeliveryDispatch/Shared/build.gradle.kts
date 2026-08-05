plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
}

kotlin {
    jvmToolchain(22)
}
