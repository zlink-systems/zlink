plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
    api("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
}

kotlin.jvmToolchain(22)
