//
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//

#ifndef YB_UTIL_STATUS_FORMAT_H
#define YB_UTIL_STATUS_FORMAT_H

// TODO Should be migrated to separate header
// #include "yb/gutil/strings/substitute.h"

// #include "yb/util/format.h"
// #include "yb/util/status.h"

#define STATUS_SUBSTITUTE(status_type, ...) \
    (Status(Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            strings::Substitute(__VA_ARGS__)))

#define STATUS_FORMAT(status_type, ...) \
    (::yb::Status(::yb::Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            ::yb::Format(__VA_ARGS__)))

#define STATUS_EC_FORMAT(status_type, error_code, ...) \
    (::yb::Status(::yb::Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            ::yb::Format(__VA_ARGS__), error_code))

#define SCHECK_FORMAT(expr, status_type, msg, ...) do { \
    if (PREDICT_FALSE(!(expr))) return STATUS_FORMAT(status_type, (msg), __VA_ARGS__); \
  } while (0)

#define SCHECK_OP(var1, op, var2, status_type, msg) \
  do { \
    auto v1_tmp = (var1); \
    auto v2_tmp = (var2); \
    if (PREDICT_FALSE(!((v1_tmp) op (v2_tmp)))) return STATUS(status_type, \
      yb::Format("$0: $1 vs. $2", (msg), v1_tmp, v2_tmp)); \
  } while (0)

#define SCHECK_EQ(var1, var2, status_type, msg) SCHECK_OP(var1, ==, var2, status_type, msg)
#define SCHECK_NE(var1, var2, status_type, msg) SCHECK_OP(var1, !=, var2, status_type, msg)
#define SCHECK_GT(var1, var2, status_type, msg) SCHECK_OP(var1, >, var2, status_type, msg)
#define SCHECK_GE(var1, var2, status_type, msg) SCHECK_OP(var1, >=, var2, status_type, msg)
#define SCHECK_LT(var1, var2, status_type, msg) SCHECK_OP(var1, <, var2, status_type, msg)
#define SCHECK_LE(var1, var2, status_type, msg) SCHECK_OP(var1, <=, var2, status_type, msg)
#define SCHECK_BOUNDS(var1, lbound, rbound, status_type, msg) \
    do { \
      SCHECK_GE(var1, lbound, status_type, msg); \
      SCHECK_LE(var1, rbound, status_type, msg); \
    } while(false)

#ifndef NDEBUG

// Debug mode ("not defined NDEBUG (non-debug-mode)" means "debug mode").
// In case the check condition is false, we will crash with a CHECK failure.

#define RSTATUS_DCHECK(expr, type, msg) DCHECK(expr) << msg
#define RSTATUS_DCHECK_EQ(var1, var2, type, msg) DCHECK_EQ(var1, var2) << msg
#define RSTATUS_DCHECK_NE(var1, var2, type, msg) DCHECK_NE(var1, var2) << msg
#define RSTATUS_DCHECK_GT(var1, var2, type, msg) DCHECK_GT(var1, var2) << msg
#define RSTATUS_DCHECK_GE(var1, var2, type, msg) DCHECK_GE(var1, var2) << msg
#define RSTATUS_DCHECK_LT(var1, var2, type, msg) DCHECK_LT(var1, var2) << msg
#define RSTATUS_DCHECK_LE(var1, var2, type, msg) DCHECK_LE(var1, var2) << msg

#else

// Release mode.
// In case the check condition is false, we will return an error status.

#define RSTATUS_DCHECK(expr, type, msg) SCHECK(expr, type, msg)
#define RSTATUS_DCHECK_EQ(var1, var2, type, msg) SCHECK_EQ(var1, var2, type, msg)
#define RSTATUS_DCHECK_NE(var1, var2, type, msg) SCHECK_NE(var1, var2, type, msg)
#define RSTATUS_DCHECK_GT(var1, var2, type, msg) SCHECK_GT(var1, var2, type, msg)
#define RSTATUS_DCHECK_GE(var1, var2, type, msg) SCHECK_GE(var1, var2, type, msg)
#define RSTATUS_DCHECK_LT(var1, var2, type, msg) SCHECK_LT(var1, var2, type, msg)
#define RSTATUS_DCHECK_LE(var1, var2, type, msg) SCHECK_LE(var1, var2, type, msg)

#endif

#endif // YB_UTIL_STATUS_FORMAT_H
