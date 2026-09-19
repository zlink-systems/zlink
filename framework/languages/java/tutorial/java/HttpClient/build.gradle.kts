plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

dependencies {
    // --8<-- [start:http-client-dependency]
    // This process is outside the mesh and references only the HTTP client package.
    implementation(libs.zlink.http.client)
    // --8<-- [end:http-client-dependency]
}

application {
    mainClass.set("systems.zlink.tutorial.httpclient.HttpClientProgram")
}
