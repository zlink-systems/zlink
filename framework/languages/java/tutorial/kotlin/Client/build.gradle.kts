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
    implementation(libs.zlink.framework.spring.boot.starter)
    // The Kotlin call surface: ZLinkRouteClient.kotlin() and the suspending await().
    implementation(libs.zlink.framework.kotlin)
    // Spring MVC bridges a suspend controller method through Reactor, so this is
    // required even though no controller body here uses a Reactor type.
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-reactor")
    // This process is the HTTP gateway the tutorial is driven through.
    implementation(libs.spring.boot.starter.web)
    implementation(libs.jackson.module.kotlin)
}

application {
    mainClass.set("systems.zlink.tutorial.client.ClientApplicationKt")
}
