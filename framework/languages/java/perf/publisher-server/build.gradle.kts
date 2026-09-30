plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "publisher-server"
    mainClass.set("systems.zlink.framework.perf.servers.publisher.Program")
}
