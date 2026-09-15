plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

tasks.withType<JavaCompile> {
    // @PathVariable resolves "name" by matching the compiled parameter name;
    // without -parameters javac drops it and every call 500s (see README).
    options.compilerArgs.add("-parameters")
}

dependencies {
    implementation(project(":java:Shared"))
    implementation(libs.zlink.framework.core)
    implementation(libs.zlink.framework.spring.boot.starter)
    // This process exposes GET /hello/{name}, so it needs the web starter
    // (the server process does not).
    implementation(libs.spring.boot.starter.web)
}

application {
    mainClass.set("systems.zlink.quickstart.client.ClientApplication")
}
