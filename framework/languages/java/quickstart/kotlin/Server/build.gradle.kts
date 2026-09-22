plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    id("org.jetbrains.kotlin.plugin.spring")
}

kotlin {
    jvmToolchain(25)
}

dependencies {
    implementation(project(":kotlin:Shared"))
    implementation(libs.zlink.framework.core)
    implementation(libs.zlink.framework.kotlin)
    // DI and lifecycle registration (EnableZLinkFramework). This process exposes no HTTP, so
    // plain spring-boot-starter is enough — no embedded web server starts.
    implementation(libs.zlink.framework.spring.boot.starter)
    implementation(libs.spring.boot.starter)
    // The mesh's default JSON codec needs this to (de)serialize the Kotlin
    // Hello/Greeting data classes — see the version comment in
    // gradle/libs.versions.toml.
    implementation(libs.jackson.module.kotlin)
}

application {
    // Top-level `fun main` in ServerApplication.kt compiles to this facade
    // class (file name + "Kt"), not the ServerApplication class itself.
    mainClass.set("systems.zlink.quickstart.server.ServerApplicationKt")
}
