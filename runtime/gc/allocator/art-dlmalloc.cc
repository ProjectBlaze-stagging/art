/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "art-dlmalloc.h"

#include <android-base/logging.h>

#include "base/bit_utils.h"

// ART specific morecore implementation defined in space.cc.
static void* art_heap_morecore(void* m, intptr_t increment);
#define MORECORE(x) art_heap_morecore(m, x)

// Custom heap error handling.
#define PROCEED_ON_ERROR 0
static void art_heap_corruption(const char* function);
static void art_heap_usage_error(const char* function, void* p);
#define CORRUPTION_ERROR_ACTION(m) art_heap_corruption(__FUNCTION__)
#define USAGE_ERROR_ACTION(m, p) art_heap_usage_error(__FUNCTION__, p)

// Ugly inclusion of C file so that ART specific #defines configure dlmalloc for our use for
// mspaces (regular dlmalloc is still declared in bionic).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wempty-body"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wnull-pointer-arithmetic"
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include "dlmalloc.c"  // NOLINT
// Note: dlmalloc.c uses a DEBUG define to drive debug code. This interferes with the DEBUG severity
//       of libbase, so undefine it now.
#undef DEBUG
#pragma GCC diagnostic pop

static void* art_heap_morecore(void* m, intptr_t increment) {
  return ::art::gc::allocator::ArtDlMallocMoreCore(m, increment);
}

static void art_heap_corruption(const char* function) {
  LOG(FATAL) << "Corrupt heap detected in: " << function;
}

static void art_heap_usage_error(const char* function, void* p) {
  LOG(FATAL) << "Incorrect use of function '" << function << "' argument " << p
      << " not expected";
}

#include <sys/mman.h>

#include "base/utils.h"
#include "runtime_globals.h"

extern "C" void DlmallocMadviseCallback(void* start, void* end, size_t used_bytes, void* arg) {
  // Is this chunk in use?
  if (used_bytes != 0) {
    return;
  }
  // Do we have any whole pages to give back?
  start = reinterpret_cast<void*>(art::RoundUp(reinterpret_cast<uintptr_t>(start), art::gPageSize));
  end = reinterpret_cast<void*>(art::RoundDown(reinterpret_cast<uintptr_t>(end), art::gPageSize));
  if (end > start) {
    size_t length = reinterpret_cast<uint8_t*>(end) - reinterpret_cast<uint8_t*>(start);
    int rc = madvise(start, length, MADV_DONTNEED);
    if (UNLIKELY(rc != 0)) {
      errno = rc;
      PLOG(FATAL) << "madvise failed during heap trimming";
    }
    size_t* reclaimed = reinterpret_cast<size_t*>(arg);
    *reclaimed += length;
  }
}

extern "C" void DlmallocBytesAllocatedCallback([[maybe_unused]] void* start,
                                               [[maybe_unused]] void* end,
                                               size_t used_bytes,
                                               void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* bytes_allocated = reinterpret_cast<size_t*>(arg);
  *bytes_allocated += used_bytes + sizeof(size_t);
}

extern "C" void DlmallocObjectsAllocatedCallback([[maybe_unused]] void* start,
                                                 [[maybe_unused]] void* end,
                                                 size_t used_bytes,
                                                 void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* objects_allocated = reinterpret_cast<size_t*>(arg);
  ++(*objects_allocated);
}
