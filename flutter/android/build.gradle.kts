import java.io.IOException
import java.net.URI
import java.security.MessageDigest

group = "tech.soit.lychee.lychee_player"
version = "1.0-SNAPSHOT"

buildscript {
    val kotlinVersion = "2.4.0"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

// The core comes from the Android SDK's AAR, io.github.boyan01.lychee:lychee
// (the FFmpeg-free liblychee.so and the frame driver). In this repository
// that is sdk/android/build/repo, from `sdk/android/gradlew -p sdk/android
// :lychee:publish`; otherwise the release that ../prebuilt.json pins
// (tool/release.sh), downloaded once into ~/.cache/lychee_player as a Maven
// repository and checked against its SHA-256s. LYCHEE_PREBUILT_URL replaces
// the release URL with a mirror's. The app resolves the plugin's
// dependencies, so every project gets the repository.
// The app can persist this in android/gradle.properties; CI can override it.
val backend = providers.environmentVariable("LYCHEE_WITH_FFMPEG")
    .orElse(providers.gradleProperty("LYCHEE_WITH_FFMPEG"))
    .orElse("OFF").get().uppercase()
require(backend in listOf("ON", "OFF")) { "LYCHEE_WITH_FFMPEG must be ON or OFF" }
val withFFmpeg = backend == "ON"
val artifacts = if (withFFmpeg) listOf("lychee", "lychee-ffmpeg") else listOf("lychee")
val localRepo = file("../../sdk/android/build/repo")
val checkoutVersion = Regex("""(?m)^version: (\S+)""")
    .find(file("../pubspec.yaml").readText())!!.groupValues[1]
// A directory can hold an older SDK or only the base AAR. Use local builds
// only when the selected artifacts are complete for this exact version.
val completeLocalRepo = artifacts.all { artifact ->
    val dir = File(localRepo, "io/github/boyan01/lychee/$artifact/$checkoutVersion")
    listOf("aar", "pom").all { File(dir, "$artifact-$checkoutVersion.$it").isFile }
}
val (lycheeRepo, lycheeVersion) = if (completeLocalRepo) {
    localRepo to checkoutVersion
} else {
    fetchPrebuiltAar()
}

fun fetchPrebuiltAar(): Pair<File, String> {
    @Suppress("UNCHECKED_CAST")
    val manifest = groovy.json.JsonSlurper().parse(file("../prebuilt.json")) as Map<String, Any?>
    val tag = manifest["tag"] as String?
        ?: throw GradleException("lychee_player: no $localRepo and no release in prebuilt.json; " +
            "run sdk/android/gradlew -p sdk/android :lychee:publish")
    @Suppress("UNCHECKED_CAST")
    val files = manifest["files"] as Map<String, String>
    val url = System.getenv("LYCHEE_PREBUILT_URL")?.takeIf { it.isNotEmpty() } ?: manifest["url"] as String
    val cacheHome = System.getenv("XDG_CACHE_HOME")?.takeIf { it.isNotEmpty() }?.let(::File)
        ?: File(System.getProperty("user.home"), ".cache")
    val repo = File(cacheHome, "lychee_player/$tag/maven")
    val version = tag.removePrefix("v")
    fun sha256(f: File) = MessageDigest.getInstance("SHA-256")
        .digest(f.readBytes()).joinToString("") { "%02x".format(it) }
    for (artifact in artifacts) {
        val dir = File(repo, "io/github/boyan01/lychee/$artifact/$version")
        for (name in listOf("$artifact-$version.aar", "$artifact-$version.pom")) {
            val sha = files[name] ?: throw GradleException("lychee_player: prebuilt.json has no $name")
            val target = File(dir, name)
            if (target.isFile && sha256(target) == sha) continue
            dir.mkdirs()
            val from = "$url/$tag/$name"
            logger.lifecycle("lychee_player: downloading $from")
            val part = File(dir, "$name.part")
            var error: Exception? = null
            for (attempt in 1..3) {
                try {
                    URI(from).toURL().openStream().use { input ->
                        part.outputStream().use { input.copyTo(it) }
                    }
                    error = null
                    break
                } catch (e: IOException) {
                    error = e
                }
            }
            if (error != null) {
                throw GradleException("lychee_player: cannot download $from; LYCHEE_PREBUILT_URL can name a mirror", error)
            }
            val got = sha256(part)
            if (got != sha) {
                part.delete()
                throw GradleException("lychee_player: $from has SHA-256 $got, not $sha")
            }
            part.renameTo(target)
        }
    }
    return repo to version
}

rootProject.allprojects {
    repositories {
        google()
        mavenCentral()
        maven { url = uri(lycheeRepo) }
    }
}

plugins {
    id("com.android.library")
}

android {
    namespace = "tech.soit.lychee.lychee_player"

    compileSdk = 36

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
        }
    }

    buildFeatures { buildConfig = true }

    defaultConfig {
        consumerProguardFiles("consumer-rules.pro")
        minSdk = 26
        buildConfigField("boolean", "LYCHEE_WITH_FFMPEG", withFFmpeg.toString())
    }
}

dependencies {
    implementation("io.github.boyan01.lychee:lychee:$lycheeVersion")
    if (withFFmpeg) implementation("io.github.boyan01.lychee:lychee-ffmpeg:$lycheeVersion")
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}
