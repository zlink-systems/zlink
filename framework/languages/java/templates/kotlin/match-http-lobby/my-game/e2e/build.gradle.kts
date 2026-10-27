plugins { application }

dependencies {
    implementation(project(":server:shared"))
    implementation(project(":server:match"))
    implementation(project(":server:lobby"))
    implementation("systems.zlink:zlink-stream-connector:${rootProject.extra["frameworkVersion"]}")
}

val launcher = javaToolchains.launcherFor { languageVersion.set(JavaLanguageVersion.of(25)) }

tasks.register("javaHome") {
    doLast {
        val destination = rootProject.file(".logs/java-home.txt")
        destination.parentFile.mkdirs()
        destination.writeText(launcher.get().metadata.installationPath.asFile.absolutePath)
    }
}

application {
    mainClass.set("MyGame.e2e.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
