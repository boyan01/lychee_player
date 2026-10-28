#ifndef LYCHEE_BACKENDS_ANDROID_HTTP_DATA_SOURCE_H_
#define LYCHEE_BACKENDS_ANDROID_HTTP_DATA_SOURCE_H_

#include <memory>
#include <string>

#include "media/data_source.h"

namespace lychee {

// Whether HttpDataSource can open anything: there is a Java VM.
bool HttpDataSourceAvailable();

// http(s) through Java's HttpURLConnection, over JNI: the platform's
// proxies, CAs and network security config, as NSURLSession gives Apple
// (backends/apple/http_data_source.mm, whose behaviour this follows).
// Reads block on the caller's thread: one streaming request from the
// offset read, replaced by a range request when a read jumps elsewhere,
// with a few retries when a connection drops.
std::unique_ptr<DataSource> OpenHttpDataSource(const std::string& url,
                                               std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_HTTP_DATA_SOURCE_H_
