// This Class Factor From The Chromium Project.
// chromium/base/location.h

// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_LOCATION_H_
#define MEDIA_BASE_LOCATION_H_

#include <string>

namespace media {

namespace tracked_objects {

// Location provides basic info where of an object was constructed, or was
// significantly brought to life.
class Location {
 public:

  // Constructor should be called with a long-lived char*, such as __FILE__.
  // It assumes the provided value will persist as a global constant, and it
  // will not make a copy of it.
  Location(const char *function_name,
           const char *file_name,
           int line_number,
           const void *program_counter);

  // Provide a default constructor for easy of debugging.
  Location();

  // Comparison operator for insertion into a std::map<> hash tables.
  // All we need is *some* (any) hashing distinction.  Strings should already
  // be unique, so we don't bother with strcmp or such.
  // Use line number as the primary key (because it is fast, and usually gets us
  // a difference), and then pointers as secondary keys (just to get some
  // distinctions).
  bool operator<(const Location &other) const {
    if (line_number_ != other.line_number_)
      return line_number_ < other.line_number_;
    if (file_name_ != other.file_name_)
      return file_name_ < other.file_name_;
    return function_name_ < other.function_name_;
  }

  const char *function_name() const { return function_name_; }
  const char *file_name() const { return file_name_; }
  int line_number() const { return line_number_; }
  const void *program_counter() const { return program_counter_; }

  std::string ToString() const;

  std::string ToShortString() const;

 private:
  const char *function_name_;
  const char *file_name_;
  int line_number_;
  const void *program_counter_;
};

const void* GetProgramCounter();


// Define a macro to record the current source location.
#define FROM_HERE FROM_HERE_WITH_EXPLICIT_FUNCTION(__FUNCTION__)

#define FROM_HERE_WITH_EXPLICIT_FUNCTION(function_name)                               \
    ::media::tracked_objects::Location(function_name,                                 \
                                       __FILE__,                                      \
                                       __LINE__,                                      \
                                       ::media::tracked_objects::GetProgramCounter())


}
}

#endif //MEDIA_BASE_LOCATION_H_
