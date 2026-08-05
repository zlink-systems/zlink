plugins {
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(22)
}

dependencies {
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}
