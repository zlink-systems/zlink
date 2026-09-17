plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

tasks.withType<JavaCompile> {
    // @PathVariable/@RequestParam resolve "channel" and "value" by matching the
    // compiled parameter name; without -parameters javac drops it (see README).
    options.compilerArgs.add("-parameters")
}

dependencies {
    implementation(project(":java:Shared"))
    implementation(libs.zlink.framework.core)
    // Spot 단계가 쓰는 Location Store·Relocation Store 구현이다.
    implementation(libs.zlink.framework.locations.redis)
    // DI and lifecycle registration (EnableZLinkFramework).
    implementation(libs.zlink.framework.spring.boot.starter)
    // The admin endpoint that changes a channel weight at runtime is the only HTTP
    // this process exposes, and it is why the web starter is here.
    implementation(libs.spring.boot.starter.web)
}

application {
    mainClass.set("systems.zlink.tutorial.server.ServerApplication")
}
