plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.9.0")
    api("systems.zlink:zlink-framework-kotlin:0.9.0")
}

kotlin {
    jvmToolchain(22)
}
