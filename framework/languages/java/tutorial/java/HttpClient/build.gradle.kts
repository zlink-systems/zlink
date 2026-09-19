plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

dependencies {
    // This process is outside the mesh and references only the HTTP client package.
    implementation(libs.zlink.http.client)
}

application {
    mainClass.set("systems.zlink.tutorial.httpclient.HttpClientProgram")
}
