plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}
