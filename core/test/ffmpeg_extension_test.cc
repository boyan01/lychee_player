#include "media/ffmpeg_extension.h"

#include <string>

#include "test.h"

using lychee::FFmpegExtension;

TEST(FFmpegExtension, MissingLibrary) {
  FFmpegExtension extension;
  std::string error;
  EXPECT_TRUE(!extension.Load("liblychee_not_installed.so", &error));
  EXPECT_TRUE(error.find("Cannot load") != std::string::npos);
  EXPECT_TRUE(extension.backend() == nullptr);
}

TEST(FFmpegExtension, RejectsUnrelatedLibrary) {
  FFmpegExtension extension;
  std::string error;
  EXPECT_TRUE(!extension.Load("liblog.so", &error));
  EXPECT_TRUE(error.find("Incompatible") != std::string::npos);
  EXPECT_TRUE(extension.backend() == nullptr);
}

TEST(FFmpegExtension, RejectsDifferentBuild) {
  FFmpegExtension extension;
  std::string error;
  EXPECT_TRUE(!extension.Load("./liblychee_ffmpeg_mismatch.so", &error));
  EXPECT_TRUE(error.find("Incompatible") != std::string::npos);
  EXPECT_TRUE(extension.backend() == nullptr);
  // A failed attempt does not activate or seal the backend.
  EXPECT_TRUE(!extension.Load("liblychee_not_installed.so", &error));
  EXPECT_TRUE(error.find("Cannot load") != std::string::npos);
}

TEST(FFmpegExtension, RejectsLoadAfterPlayerCreation) {
  FFmpegExtension extension;
  extension.Seal();
  std::string error;
  EXPECT_TRUE(!extension.Load("liblychee_not_installed.so", &error));
  EXPECT_TRUE(error.find("before creating the first player") !=
              std::string::npos);
  EXPECT_TRUE(extension.backend() == nullptr);
}
