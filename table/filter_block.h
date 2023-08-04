// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A filter block is stored near the end of a Table file.  It contains
// filters (e.g., bloom filters) for all data blocks in the table combined
// into a single filter block.

#ifndef STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_

#include "db/dbformat.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "leveldb/env.h"
#include "leveldb/slice.h"

#include "table/format.h"
#include "util/hash.h"
#include "util/mutexlock.h"
#include <atomic>

namespace leveldb {

class FilterPolicy;

// Generate 4 filters and load 1 filter when FilterBlockReader is created
static const size_t kLoadFilterUnitsNumber = 2;
static const size_t kAllFilterUnitsNumber  = 6;
static const uint64_t kLifeTime            = 10000;

// A FilterBlockBuilder is used to construct all of the filters for a
// particular Table.  It generates a single string which is stored as
// a special block in the Table.
//
// The sequence of calls to FilterBlockBuilder must match the regexp:
//      (StartBlock AddKey*)* Finish
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy*);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  Slice Finish(const BlockHandle& handle);
  const std::vector<std::string>& ReturnFilters();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  std::string keys_;             // Flattened key contents
  std::vector<size_t> start_;    // Starting index in keys_ of each key
  std::string result_;           // Filter data computed so far
  std::vector<Slice> tmp_keys_;  // policy_->CreateFilter() argument
  std::vector<uint32_t> filter_offsets_;

  std::vector<std::string>
      filter_units_;  // filter units bitmap generated by builder
};

class FilterBlockReader {
 public:
  // REQUIRES: "contents" and *policy must stay live while *this is live.
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents,
                    RandomAccessFile* file);
  bool KeyMayMatch(uint64_t block_offset, const Slice& key);
  Status LoadFilter();
  Status EvictFilter();
  Status InitLoadFilter();
  Status GoBackToInitFilter(RandomAccessFile* file);
  ~FilterBlockReader();

  void UpdateState(const SequenceNumber& sn);

  size_t LoadFilterNumber() const { return init_units_number_; }

  size_t FilterUnitsNumber()  {
    MutexLock l(&mutex_);
    return FilterUnitsNumberInternal();
  }

  size_t FilterUnitsNumberInternal() {
    mutex_.AssertHeld();
    WaitForLoading();
    return filter_units.size();
  }

  void SetAccessTime(uint64_t access_time){
    // we only set access time after compaction, before Get()
    assert(access_time_.load(std::memory_order_relaxed) == 0);
    access_time_.store(access_time, std::memory_order_relaxed);
  }

  uint64_t AccessTime() const {
    return access_time_.load(std::memory_order_relaxed);
  }

  bool IsCold(SequenceNumber now_sequence) {
    return now_sequence >= (sequence_.load(std::memory_order_relaxed) + kLifeTime);
  }

  size_t OneUnitSize() const { return disk_size_; }

  bool CanBeLoaded() {
    MutexLock l(&mutex_);
    return FilterUnitsNumberInternal() < all_units_number_;
  }

  bool CanBeEvict() {
    MutexLock l(&mutex_);
    return FilterUnitsNumberInternal() > 0;
  }

  // filter block memory overhead(Byte)
  size_t Size() {
    MutexLock l(&mutex_);
    return FilterUnitsNumberInternal() * disk_size_;
  }

  // R: (r)^n
  // IO: R*F
  double IOs() {
    MutexLock l(&mutex_);
    double fpr = pow(policy_->FalsePositiveRate(),
                     static_cast<double>(FilterUnitsNumberInternal()));
    return fpr * static_cast<double>(access_time_);
  }

  double LoadIOs() {
    MutexLock l(&mutex_);
    double fpr = pow(policy_->FalsePositiveRate(),
                     static_cast<double>(FilterUnitsNumberInternal() + 1));
    return fpr * static_cast<double>(access_time_);
  }

  double EvictIOs() {
    MutexLock l(&mutex_);
    assert(!filter_units.empty());
    double fpr = pow(policy_->FalsePositiveRate(),
                     static_cast<double>(FilterUnitsNumberInternal() - 1));
    return fpr * static_cast<double>(access_time_);
  }

 private:
  const FilterPolicy* policy_;
  const char* data_;  // Pointer to filter meta data (at block-start)

  const char* offset_;    // Pointer to beginning of offset array (at block-end)
  uint64_t disk_offset_;  // first filter unit offset in disk
  uint32_t disk_size_;    // filter units' size, every bitmap has same size
  uint32_t init_units_number_;  // the number of filter units to load when
                                // filter block is created
  uint32_t all_units_number_;   // the number of filter units
  size_t base_lg_;  // Encoding parameter (see kFilterBaseLg in .cc file)
  size_t num_;      // Number of entries in offset array

  mutable port::Mutex mutex_;
  std::atomic<uint64_t> access_time_;
  std::atomic<SequenceNumber> sequence_;

  RandomAccessFile* file_ GUARDED_BY(mutex_);

  std::vector<const char*> filter_units GUARDED_BY(mutex_);
  std::vector<ReadBuffer*> reader_buffers_ GUARDED_BY(mutex_);

  Status LoadFilterInternal();
  Status EvictFilterInternal();

  void UpdateFile(RandomAccessFile* file);

  bool init_done GUARDED_BY(mutex_);
  port::CondVar init_signal GUARDED_BY(mutex_);

  // main thread maybe read unloaded filterblockreader
  // when background thread does not finish
  // waiting for background thread signal
  void WaitForLoading(){
    mutex_.AssertHeld();
    while (!init_done){
      init_signal.Wait();
    }
  }
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
