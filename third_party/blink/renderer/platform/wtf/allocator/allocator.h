// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_ALLOCATOR_ALLOCATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_ALLOCATOR_ALLOCATOR_H_

#include <atomic>

#include "build/build_config.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/wtf/assertions.h"
#include "third_party/blink/renderer/platform/wtf/type_traits.h"

namespace WTF {

namespace internal {
// A dummy class used in following macros.
class __thisIsHereToForceASemicolonAfterThisMacro;
}  // namespace internal

// Classes that contain references to garbage-collected objects but aren't
// themselves garbaged allocated, have some extra macros available which
// allows their use to be restricted to cases where the garbage collector
// is able to discover their references. These macros will be useful for
// non-garbage-collected objects to avoid unintended allocations.
//
// STACK_ALLOCATED(): Use if the object is only stack allocated.
// Garbage-collected objects should be in Members but you do not need the
// trace method as they are on the stack.  (Down the line these might turn
// in to raw pointers, but for now Members indicate that we have thought
// about them and explicitly taken care of them.)
//
// DISALLOW_NEW(): Cannot be allocated with new operators but can be a
// part of object, a value object in collections or stack allocated. If it has
// Members you need a trace method and the containing object needs to call that
// trace method.
//
#define DISALLOW_NEW()                                                        \
 public:                                                                      \
  using IsDisallowNewMarker = int;                                            \
  void* operator new(size_t, NotNullTag, void* location) { return location; } \
  void* operator new(size_t, void* location) { return location; }             \
                                                                              \
 private:                                                                     \
  void* operator new(size_t) = delete;                                        \
                                                                              \
 public:                                                                      \
  friend class ::WTF::internal::__thisIsHereToForceASemicolonAfterThisMacro

#define STATIC_ONLY(Type)                                 \
  Type() = delete;                                        \
  Type(const Type&) = delete;                             \
  Type& operator=(const Type&) = delete;                  \
  void* operator new(size_t) = delete;                    \
  void* operator new(size_t, NotNullTag, void*) = delete; \
  void* operator new(size_t, void*) = delete

#define IS_GARBAGE_COLLECTED_TYPE()         \
 public:                                    \
  using IsGarbageCollectedTypeMarker = int; \
                                            \
 private:                                   \
  friend class ::WTF::internal::__thisIsHereToForceASemicolonAfterThisMacro

#define IS_GARBAGE_COLLECTED_CONTAINER_TYPE()         \
  IS_GARBAGE_COLLECTED_TYPE();                        \
                                                      \
 public:                                              \
  using IsGarbageCollectedCollectionTypeMarker = int; \
                                                      \
 private:                                             \
  friend class ::WTF::internal::__thisIsHereToForceASemicolonAfterThisMacro

#if defined(__clang__)
#define ANNOTATE_STACK_ALLOCATED \
  __attribute__((annotate("blink_stack_allocated")))
#else
#define ANNOTATE_STACK_ALLOCATED
#endif

#define STACK_ALLOCATED()                                       \
 public:                                                        \
  using IsStackAllocatedTypeMarker [[maybe_unused]] = int;      \
                                                                \
 private:                                                       \
  ANNOTATE_STACK_ALLOCATED void* operator new(size_t) = delete; \
  void* operator new(size_t, NotNullTag, void*) = delete;       \
  void* operator new(size_t, void*) = delete

// Provides customizable overrides of fastMalloc/fastFree and operator
// new/delete
//
// Provided functionality:
//    Macro: USING_FAST_MALLOC
//
// Example usage:
//    class Widget {
//        USING_FAST_MALLOC(Widget)
//    ...
//    };
//
//    struct Data {
//        USING_FAST_MALLOC(Data)
//    public:
//    ...
//    };
//

#define USING_FAST_MALLOC_INTERNAL(type, typeName)                    \
 public:                                                              \
  void* operator new(size_t, void* p) { return p; }                   \
  void* operator new[](size_t, void* p) { return p; }                 \
                                                                      \
  void* operator new(size_t size) {                                   \
    return ::WTF::Partitions::FastMalloc(size, typeName);             \
  }                                                                   \
                                                                      \
  void operator delete(void* p) { ::WTF::Partitions::FastFree(p); }   \
                                                                      \
  void* operator new[](size_t size) {                                 \
    return ::WTF::Partitions::FastMalloc(size, typeName);             \
  }                                                                   \
                                                                      \
  void operator delete[](void* p) { ::WTF::Partitions::FastFree(p); } \
  void* operator new(size_t, NotNullTag, void* location) {            \
    DCHECK(location);                                                 \
    return location;                                                  \
  }                                                                   \
                                                                      \
 private:                                                             \
  friend class ::WTF::internal::__thisIsHereToForceASemicolonAfterThisMacro

// In official builds, do not include type info string literals to avoid
// bloating the binary.
#if defined(OFFICIAL_BUILD)
#define WTF_HEAP_PROFILER_TYPE_NAME(T) nullptr
#else
#define WTF_HEAP_PROFILER_TYPE_NAME(T) ::WTF::GetStringWithTypeName<T>()
#endif

// Both of these macros enable fast malloc and provide type info to the heap
// profiler. The regular macro does not provide type info in official builds,
// to avoid bloating the binary with type name strings. The |WITH_TYPE_NAME|
// variant provides type info unconditionally, so it should be used sparingly.
// Furthermore, the |WITH_TYPE_NAME| variant does not work if |type| is a
// template argument; |USING_FAST_MALLOC| does.
#define USING_FAST_MALLOC(type) \
  USING_FAST_MALLOC_INTERNAL(type, WTF_HEAP_PROFILER_TYPE_NAME(type))
#define USING_FAST_MALLOC_WITH_TYPE_NAME(type) \
  USING_FAST_MALLOC_INTERNAL(type, #type)

// TOOD(omerkatz): replace these casts with std::atomic_ref (C++20) once it
// becomes available
template <typename T>
ALWAYS_INLINE std::atomic<T>* AsAtomicPtr(T* t) {
  return reinterpret_cast<std::atomic<T>*>(t);
}
template <typename T>
ALWAYS_INLINE const std::atomic<T>* AsAtomicPtr(const T* t) {
  return reinterpret_cast<const std::atomic<T>*>(t);
}

// Load |bytes| bytes from |from| to |to| using atomic reads. Assumes |to| is
// size_t-aligned and  points to a buffer of size at least |bytes|. Note that
// atomicity is guaranteed only per word, not for the entire |bytes| bytes as
// a whole.
WTF_EXPORT void AtomicMemcpy(void* to, const void* from, size_t bytes);
template <size_t bytes>
ALWAYS_INLINE void AtomicMemcpy(void* to, const void* from) {
  AtomicMemcpy(to, from, bytes);
}

// AtomicMemcpy specializations:

#if defined(ARCH_CPU_X86_64)
template <>
ALWAYS_INLINE void AtomicMemcpy<sizeof(uint32_t)>(void* to, const void* from) {
  *reinterpret_cast<uint32_t*>(to) =
      AsAtomicPtr(reinterpret_cast<const uint32_t*>(from))
          ->load(std::memory_order_relaxed);
}
#endif  // ARCH_CPU_X86_64

template <>
ALWAYS_INLINE void AtomicMemcpy<sizeof(size_t)>(void* to, const void* from) {
  *reinterpret_cast<size_t*>(to) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from))
          ->load(std::memory_order_relaxed);
}

template <>
ALWAYS_INLINE void AtomicMemcpy<2 * sizeof(size_t)>(void* to,
                                                    const void* from) {
  *reinterpret_cast<size_t*>(to) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from))
          ->load(std::memory_order_relaxed);
  *(reinterpret_cast<size_t*>(to) + 1) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from) + 1)
          ->load(std::memory_order_relaxed);
}

template <>
ALWAYS_INLINE void AtomicMemcpy<3 * sizeof(size_t)>(void* to,
                                                    const void* from) {
  *reinterpret_cast<size_t*>(to) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from))
          ->load(std::memory_order_relaxed);
  *(reinterpret_cast<size_t*>(to) + 1) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from) + 1)
          ->load(std::memory_order_relaxed);
  *(reinterpret_cast<size_t*>(to) + 2) =
      AsAtomicPtr(reinterpret_cast<const size_t*>(from) + 2)
          ->load(std::memory_order_relaxed);
}

}  // namespace WTF

// This version of placement new omits a 0 check.
enum NotNullTag { NotNull };
inline void* operator new(size_t, NotNullTag, void* location) {
  DCHECK(location);
  return location;
}

#endif /* WTF_Allocator_h */
