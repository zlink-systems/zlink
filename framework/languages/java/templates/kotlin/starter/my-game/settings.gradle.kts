plugins { id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0" }

dependencyResolutionManagement { repositories { mavenCentral() } }

rootProject.name = "my-game"

include("server:shared", "server:session", "e2e")
