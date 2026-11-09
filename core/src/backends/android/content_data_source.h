#ifndef LYCHEE_BACKENDS_ANDROID_CONTENT_DATA_SOURCE_H_
#define LYCHEE_BACKENDS_ANDROID_CONTENT_DATA_SOURCE_H_

#include <memory>
#include <string>

#include "media/data_source.h"

namespace lychee {

// A content:// URI (the Storage Access Framework's documents, a USB
// drive's, a FileProvider's), opened through
// ContentResolver.openFileDescriptor(uri, "r") over JNI with the
// Context the SDK handed the core (LycheePlayer.setContext()). A
// regular file is read in place with pread(); a pipe (a provider that
// streams) is read from the start, again for a read behind what it
// keeps, and reports itself unseekable.
std::unique_ptr<DataSource> OpenContentDataSource(const std::string& url,
                                                  std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_CONTENT_DATA_SOURCE_H_
