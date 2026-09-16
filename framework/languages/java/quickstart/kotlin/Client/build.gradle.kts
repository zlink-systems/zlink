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
    implementation(libs.zlink.framework.spring.boot.starter)
    // coroutine idiom — kotlinx.coroutines.future.await() on ZLinkRequestCall's
    // CompletionStage, used by the HelloController below.
    implementation(libs.zlink.framework.kotlin)
    // Spring MVC bridges a suspend controller method through Reactor, so this
    // is required even though the handler body never uses Reactor types.
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-reactor")
    // This process exposes GET /hello/{name}, so it needs the web starter
    // (the server process does not).
    implementation(libs.spring.boot.starter.web)
}

application {
    mainClass.set("systems.zlink.quickstart.client.ClientApplicationKt")
}
