//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/util/thread_local.h"


#include <vector>
#include <algorithm>

#include "yb/rocksdb/util/mutexlock.h"
#include "yb/rocksdb/port/likely.h"

namespace rocksdb {

#if ROCKSDB_SUPPORT_THREAD_LOCAL
__thread ThreadLocalPtr::ThreadData* ThreadLocalPtr::StaticMeta::tls_ = nullptr;
#endif

// Windows doesn't support a per-thread destructor with its
// TLS primitives.  So, we build it manually by inserting a
// function to be called on each thread's exit.
// See http://www.codeproject.com/Articles/8113/Thread-Local-Storage-The-C-Way
// and http://www.nynaeve.net/?p=183
//
// really we do this to have clear conscience since using TLS with thread-pools
// is iffy
// although OK within a request. But otherwise, threads have no identity in its
// modern use.

// This runs on windows only called from the System Loader
#ifdef OS_WIN

// Windows cleanup routine is invoked from a System Loader with a different
// signature so we can not directly hookup the original OnThreadExit which is
// private member
// so we make StaticMeta class share with the us the address of the function so
// we can invoke it.
namespace wintlscleanup {

// This is set to OnThreadExit in StaticMeta singleton constructor
UnrefHandler thread_local_inclass_routine = nullptr;
pthread_key_t thread_local_key = -1;

// Static callback function to call with each thread termination.
void NTAPI WinOnThreadExit(PVOID module, DWORD reason, PVOID reserved) {
  // We decided to punt on PROCESS_EXIT
  if (DLL_THREAD_DETACH == reason) {
    if (thread_local_key != -1 && thread_local_inclass_routine != nullptr) {
      void* tls = pthread_getspecific(thread_local_key);
      if (tls != nullptr) {
        thread_local_inclass_routine(tls);
      }
    }
  }
}

}  // namespace wintlscleanup

#ifdef _WIN64

#pragma comment(linker, "/include:_tls_used")
#pragma comment(linker, "/include:p_thread_callback_on_exit")

#else  // _WIN64

#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_p_thread_callback_on_exit")

#endif  // _WIN64

// extern "C" suppresses C++ name mangling so we know the symbol name for the
// linker /INCLUDE:symbol pragma above.
extern "C" {

// The linker must not discard thread_callback_on_exit.  (We force a reference
// to this variable with a linker /include:symbol pragma to ensure that.) If
// this variable is discarded, the OnThreadExit function will never be called.
#ifdef _WIN64

// .CRT section is merged with .rdata on x64 so it must be constant data.
#pragma const_seg(".CRT$XLB")
// When defining a const variable, it must have external linkage to be sure the
// linker doesn't discard it.
extern const PIMAGE_TLS_CALLBACK p_thread_callback_on_exit;
const PIMAGE_TLS_CALLBACK p_thread_callback_on_exit =
    wintlscleanup::WinOnThreadExit;
// Reset the default section.
#pragma const_seg()

#else  // _WIN64

#pragma data_seg(".CRT$XLB")
PIMAGE_TLS_CALLBACK p_thread_callback_on_exit = wintlscleanup::WinOnThreadExit;
// Reset the default section.
#pragma data_seg()

#endif  // _WIN64

}  // extern "C"

#endif  // OS_WIN

void ThreadLocalPtr::InitSingletons() {
  ThreadLocalPtr::StaticMeta::InitSingletons();
}

ThreadLocalPtr::StaticMeta* ThreadLocalPtr::Instance() {
  // Here we prefer function static variable instead of global
  // static variable as function static variable is initialized
  // when the function is first call.  As a result, we can properly
  // control their construction order by properly preparing their
  // first function call.
  //
  // Note that here we decide to make "inst" a static pointer w/o deleting
  // it at the end instead of a static variable.  This is to avoid the following
  // destruction order desester happens when a child thread using ThreadLocalPtr
  // dies AFTER the main thread dies:  When a child thread happens to use
  // ThreadLocalPtr, it will try to delete its thread-local data on its
  // OnThreadExit when the child thread dies.  However, OnThreadExit depends
  // on the following variable.  As a result, if the main thread dies before any
  // child thread happen to use ThreadLocalPtr dies, then the destruction of
  // the following variable will go first, then OnThreadExit, therefore causing
  // invalid access.
  //
  // The above problem can be solved by using thread_local to store tls_ instead
  // of using __thread.  The major difference between thread_local and __thread
  // is that thread_local supports dynamic construction and destruction of
  // non-primitive typed variables.  As a result, we can guarantee the
  // desturction order even when the main thread dies before any child threads.
  // However, thread_local requires gcc 4.8 and is not supported in all the
  // compilers that accepts -std=c++11 (e.g., the default clang on Mac), while
  // the current RocksDB still accept gcc 4.7.
  static ThreadLocalPtr::StaticMeta* inst = new ThreadLocalPtr::StaticMeta();
  return inst;
}

void ThreadLocalPtr::StaticMeta::InitSingletons() { Mutex(); }

port::Mutex* ThreadLocalPtr::StaticMeta::Mutex() { return &Instance()->mutex_; }

void ThreadLocalPtr::StaticMeta::OnThreadExit(void* ptr) {
  auto* tls = static_cast<ThreadData*>(ptr);
  assert(tls != nullptr);

  // Use the cached StaticMeta::Instance() instead of directly calling
  // the variable inside StaticMeta::Instance() might already go out of
  // scope here in case this OnThreadExit is called after the main thread
  // dies.
  auto* inst = tls->inst;
  pthread_setspecific(inst->pthread_key_, nullptr);

  MutexLock l(inst->MemberMutex());
  inst->RemoveThreadData(tls);
  // Unref stored pointers of current thread from all instances
  uint32_t id = 0;
  for (auto& e : tls->entries) {
    void* raw = e.ptr.load();
    if (raw != nullptr) {
      auto unref = inst->GetHandler(id);
      if (unref != nullptr) {
        unref(raw);
      }
    }
    ++id;
  }
  // Delete thread local structure no matter if it is Mac platform
  delete tls;
}

ThreadLocalPtr::StaticMeta::StaticMeta() : next_instance_id_(0), head_(this) {
  if (pthread_key_create(&pthread_key_, &OnThreadExit) != 0) {
    abort();
  }

  // OnThreadExit is not getting called on the main thread.
  // Call through the static destructor mechanism to avoid memory leak.
  //
  // Caveats: ~A() will be invoked _after_ ~StaticMeta for the global
  // singleton (destructors are invoked in reverse order of constructor
  // _completion_); the latter must not mutate internal members. This
  // cleanup mechanism inherently relies on use-after-release of the
  // StaticMeta, and is brittle with respect to compiler-specific handling
  // of memory backing destructed statically-scoped objects. Perhaps
  // registering with atexit(3) would be more robust.
  //
// This is not required on Windows.
#if !defined(OS_WIN)
  static struct A {
    ~A() {
#if !(ROCKSDB_SUPPORT_THREAD_LOCAL)
      ThreadData* tls_ =
        static_cast<ThreadData*>(pthread_getspecific(Instance()->pthread_key_));
#endif
      if (tls_) {
        OnThreadExit(tls_);
      }
    }
  } a;
#endif  // !defined(OS_WIN)

  head_.next = &head_;
  head_.prev = &head_;

#ifdef OS_WIN
  // Share with Windows its cleanup routine and the key
  wintlscleanup::thread_local_inclass_routine = OnThreadExit;
  wintlscleanup::thread_local_key = pthread_key_;
#endif
}

void ThreadLocalPtr::StaticMeta::AddThreadData(ThreadLocalPtr::ThreadData* d) {
  Mutex()->AssertHeld();
  d->next = &head_;
  d->prev = head_.prev;
  head_.prev->next = d;
  head_.prev = d;
}

void ThreadLocalPtr::StaticMeta::RemoveThreadData(
    ThreadLocalPtr::ThreadData* d) {
  Mutex()->AssertHeld();
  d->next->prev = d->prev;
  d->prev->next = d->next;
  d->next = d->prev = d;
}

ThreadLocalPtr::ThreadData* ThreadLocalPtr::StaticMeta::GetThreadLocal() {
#if !(ROCKSDB_SUPPORT_THREAD_LOCAL)
  // Make this local variable name look like a member variable so that we
  // can share all the code below
  ThreadData* tls_ =
      static_cast<ThreadData*>(pthread_getspecific(Instance()->pthread_key_));
#endif

  if (UNLIKELY(tls_ == nullptr)) {
    auto* inst = Instance();
    tls_ = new ThreadData(inst);
    {
      // Register it in the global chain, needs to be done before thread exit
      // handler registration
      MutexLock l(Mutex());
      inst->AddThreadData(tls_);
    }
    // Even it is not OS_MACOSX, need to register value for pthread_key_ so that
    // its exit handler will be triggered.
    if (pthread_setspecific(inst->pthread_key_, tls_) != 0) {
      {
        MutexLock l(Mutex());
        inst->RemoveThreadData(tls_);
      }
      delete tls_;
      abort();
    }
  }
  return tls_;
}

void* ThreadLocalPtr::StaticMeta::Get(uint32_t id) const {
  auto* tls = GetThreadLocal();
  if (UNLIKELY(id >= tls->entries.size())) {
    return nullptr;
  }
  return tls->entries[id].ptr.load(std::memory_order_acquire);
}

void ThreadLocalPtr::StaticMeta::Reset(uint32_t id, void* ptr) {
  auto* tls = GetThreadLocal();
  if (UNLIKELY(id >= tls->entries.size())) {
    // Need mutex to protect entries access within ReclaimId
    MutexLock l(Mutex());
    tls->entries.resize(id + 1);
  }
  tls->entries[id].ptr.store(ptr, std::memory_order_release);
}

void* ThreadLocalPtr::StaticMeta::Swap(uint32_t id, void* ptr) {
  auto* tls = GetThreadLocal();
  if (UNLIKELY(id >= tls->entries.size())) {
    // Need mutex to protect entries access within ReclaimId
    MutexLock l(Mutex());
    tls->entries.resize(id + 1);
  }
  return tls->entries[id].ptr.exchange(ptr, std::memory_order_acquire);
}

bool ThreadLocalPtr::StaticMeta::CompareAndSwap(uint32_t id, void* ptr,
    void*& expected) {
  auto* tls = GetThreadLocal();
  if (UNLIKELY(id >= tls->entries.size())) {
    // Need mutex to protect entries access within ReclaimId
    MutexLock l(Mutex());
    tls->entries.resize(id + 1);
  }
  return tls->entries[id].ptr.compare_exchange_strong(
      expected, ptr, std::memory_order_release, std::memory_order_relaxed);
}

void ThreadLocalPtr::StaticMeta::Scrape(uint32_t id, autovector<void*>* ptrs,
    void* const replacement) {
  MutexLock l(Mutex());
  for (ThreadData* t = head_.next; t != &head_; t = t->next) {
    if (id < t->entries.size()) {
      void* ptr =
          t->entries[id].ptr.exchange(replacement, std::memory_order_acquire);
      if (ptr != nullptr) {
        ptrs->push_back(ptr);
      }
    }
  }
}

void ThreadLocalPtr::StaticMeta::SetHandler(uint32_t id, UnrefHandler handler) {
  MutexLock l(Mutex());
  handler_map_[id] = handler;
}

UnrefHandler ThreadLocalPtr::StaticMeta::GetHandler(uint32_t id) {
  Mutex()->AssertHeld();
  auto iter = handler_map_.find(id);
  if (iter == handler_map_.end()) {
    return nullptr;
  }
  return iter->second;
}

uint32_t ThreadLocalPtr::StaticMeta::GetId() {
  MutexLock l(Mutex());
  if (free_instance_ids_.empty()) {
    return next_instance_id_++;
  }

  uint32_t id = free_instance_ids_.back();
  free_instance_ids_.pop_back();
  return id;
}

uint32_t ThreadLocalPtr::StaticMeta::PeekId() const {
  MutexLock l(Mutex());
  if (!free_instance_ids_.empty()) {
    return free_instance_ids_.back();
  }
  return next_instance_id_;
}

std::vector<uint32_t> ThreadLocalPtr::StaticMeta::PeekIds() const {
  MutexLock l(Mutex());
  return std::vector<uint32_t>(free_instance_ids_.begin(), free_instance_ids_.end());
}

void ThreadLocalPtr::StaticMeta::ReclaimId(uint32_t id) {
  // This id is not used, go through all thread local data and release
  // corresponding value
  MutexLock l(Mutex());
  auto unref = GetHandler(id);
  for (ThreadData* t = head_.next; t != &head_; t = t->next) {
    if (id < t->entries.size()) {
      void* ptr = t->entries[id].ptr.exchange(nullptr);
      if (ptr != nullptr && unref != nullptr) {
        unref(ptr);
      }
    }
  }
  handler_map_[id] = nullptr;
  free_instance_ids_.push_back(id);
}

ThreadLocalPtr::ThreadLocalPtr(UnrefHandler handler)
    : id_(Instance()->GetId()) {
  if (handler != nullptr) {
    Instance()->SetHandler(id_, handler);
  }
}

ThreadLocalPtr::~ThreadLocalPtr() {
  Instance()->ReclaimId(id_);
}

void* ThreadLocalPtr::Get() const {
  return Instance()->Get(id_);
}

void ThreadLocalPtr::Reset(void* ptr) {
  Instance()->Reset(id_, ptr);
}

void* ThreadLocalPtr::Swap(void* ptr) {
  return Instance()->Swap(id_, ptr);
}

bool ThreadLocalPtr::CompareAndSwap(void* ptr, void*& expected) {
  return Instance()->CompareAndSwap(id_, ptr, expected);
}

void ThreadLocalPtr::Scrape(autovector<void*>* ptrs, void* const replacement) {
  Instance()->Scrape(id_, ptrs, replacement);
}

}  // namespace rocksdb
