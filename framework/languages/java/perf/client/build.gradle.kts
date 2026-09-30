plugins {
    application
}

dependencies {
    implementation(project(":shared"))
    implementation(zlinkLibs.zlink.stream.connector)
}

application {
    applicationName = "client"
    mainClass.set("systems.zlink.framework.perf.client.Program")
}
