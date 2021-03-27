//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_BASICTYPES_H_
#define MEDIA_BASE_BASICTYPES_H_


// The NSPR system headers define 64-bit as |long| when possible, except on
// Mac OS X.  In order to not have typedef mismatches, we do the same on LP64.
//
// On Mac OS X, |long long| is used for 64-bit types for compatibility with
// <inttypes.h> format macros even in the LP64 model.
#if defined(__LP64__) && !defined(__APPLE__) && !defined(__OpenBSD__)
typedef long                int64;
#else
typedef long long           int64;
#endif

// A macro to disallow the copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)

#endif //MEDIA_BASE_BASICTYPES_H_
