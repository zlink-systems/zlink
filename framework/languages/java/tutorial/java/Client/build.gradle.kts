plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

tasks.withType<JavaCompile> {
    // @PathVariable resolves "playerId" by matching the compiled parameter name;
    // without -parameters javac drops it and every call 500s (see README).
    options.compilerArgs.add("-parameters")
}

dependencies {
    implementation(project(":java:Shared"))
    implementation(libs.zlink.framework.core)
    // Spot 단계가 쓰는 Location Store·Relocation Store 구현이다.
    implementation(libs.zlink.framework.locations.redis)
    implementation(libs.zlink.framework.spring.boot.starter)
    // This process exposes the tutorial's HTTP endpoints, so it needs the web
    // starter (the server process does not).
    implementation(libs.spring.boot.starter.web)
}

application {
    mainClass.set("systems.zlink.tutorial.client.ClientApplication")
}
