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
    // Spot 단계가 쓰는 Location Store·Relocation Store 구현이다.
    implementation(libs.zlink.framework.locations.redis)
    // DI and lifecycle registration (EnableZLinkFramework).
    implementation(libs.zlink.framework.spring.boot.starter)
    // This process opens one small admin endpoint (see weight-runtime), so it needs
    // an embedded web server. Plain spring-boot-starter would start none.
    implementation(libs.spring.boot.starter.web)
    // The Kotlin surface: suspending handlers and ZLinkClient.kotlin().
    implementation(libs.zlink.framework.kotlin)
    // The mesh's default JSON codec needs this to (de)serialize the Kotlin data
    // classes in Shared, and so does the admin endpoint's reply.
    implementation(libs.jackson.module.kotlin)
}

application {
    // Top-level `fun main` in ServerApplication.kt compiles to this facade class
    // (file name + "Kt"), not the ServerApplication class itself.
    mainClass.set("systems.zlink.tutorial.server.ServerApplicationKt")
}
