// One publishing contract for the complementary Android artifacts. The
// only Gradle repository is local: `publish` must never upload to Central.
val repoRoot = rootDir.resolve("../..")
val optionalFfmpeg = project.name == "ffmpeg"
val sdkVersion = Regex("""(?m)^version: (\S+)""")
    .find(repoRoot.resolve("flutter/pubspec.yaml").readText())!!.groupValues[1]

// Kotlin sources plus the corresponding native implementation. FFmpeg's
// separately licensed source archive is linked in the notices/release.
tasks.withType<Zip>().matching { it.name == "sourceReleaseJar" }.configureEach {
    from(repoRoot.resolve("core/include")) { into("native/include") }
    if (optionalFfmpeg) {
        from(repoRoot.resolve("core/src/backends/ffmpeg")) { into("native/backends/ffmpeg") }
        from(file("CMakeLists.txt")) { into("native") }
    } else {
        from(repoRoot.resolve("core/src")) {
            exclude("backends/ffmpeg/**")
            into("native/src")
        }
        from(file("src/main/cpp")) { into("native/jni") }
        from(repoRoot.resolve("core/CMakeLists.txt")) { into("native") }
        from(repoRoot.resolve("core/cmake")) { into("native/cmake") }
    }
    from(repoRoot.resolve("LICENSE"))
    from(repoRoot.resolve("THIRD_PARTY_NOTICES"))
}

extensions.configure<PublishingExtension> {
    publications.withType<MavenPublication>().configureEach {
        pom {
            name.set(if (optionalFfmpeg) "Lychee FFmpeg extension" else "Lychee Android SDK")
            description.set(if (optionalFfmpeg)
                "Optional dynamically loaded FFmpeg backend for the Lychee Android SDK."
                else "FFmpeg-free Android media player with Kotlin and C interfaces.")
            url.set("https://github.com/boyan01/lychee_player")
            licenses {
                license {
                    name.set("MIT License")
                    url.set("https://opensource.org/license/mit")
                    distribution.set("repo")
                }
                license {
                    name.set(if (optionalFfmpeg) "GNU Lesser General Public License, version 2.1 or later"
                        else "BSD 3-Clause License")
                    url.set(if (optionalFfmpeg) "https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html"
                        else "https://opensource.org/license/bsd-3-clause")
                    distribution.set("repo")
                }
                if (optionalFfmpeg) license {
                    name.set("BSD 2-Clause License")
                    url.set("https://opensource.org/license/bsd-2-clause")
                    distribution.set("repo")
                }
            }
            developers {
                developer {
                    id.set("boyan01")
                    name.set("bin")
                    url.set("https://github.com/boyan01")
                }
            }
            scm {
                url.set("https://github.com/boyan01/lychee_player")
                connection.set("scm:git:https://github.com/boyan01/lychee_player.git")
                developerConnection.set("scm:git:ssh://git@github.com/boyan01/lychee_player.git")
                tag.set("v$sdkVersion")
            }
        }
    }
}

