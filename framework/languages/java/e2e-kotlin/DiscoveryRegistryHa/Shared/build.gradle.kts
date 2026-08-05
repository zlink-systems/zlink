plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("systems.zlink:zlink-framework-core:0.9.0")
}

kotlin {
    jvmToolchain(22)
}
