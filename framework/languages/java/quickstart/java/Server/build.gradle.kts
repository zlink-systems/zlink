plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

dependencies {
    implementation(project(":java:Shared"))
    implementation(libs.zlink.framework.core)
    // DI and lifecycle registration (EnableZLinkFramework). This process exposes no HTTP, so
    // plain spring-boot-starter is enough — no embedded web server starts.
    implementation(libs.zlink.framework.spring.boot.starter)
    implementation(libs.spring.boot.starter)
}

application {
    mainClass.set("systems.zlink.quickstart.server.ServerApplication")
}
