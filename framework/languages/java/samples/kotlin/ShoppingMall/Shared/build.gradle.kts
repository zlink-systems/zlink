plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}
