plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
}

kotlin {
    jvmToolchain(22)
}
