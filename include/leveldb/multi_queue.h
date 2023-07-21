// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//

#ifndef STORAGE_LEVELDB_INCLUDE_MULTI_QUEUE_H_
#define STORAGE_LEVELDB_INCLUDE_MULTI_QUEUE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

#include "table/filter_block.h"

namespace leveldb {

class LEVELDB_EXPORT MultiQueue;

LEVELDB_EXPORT MultiQueue* NewMultiQueue();

class LEVELDB_EXPORT MultiQueue {
 public:
  MultiQueue() = default;

  MultiQueue(const MultiQueue&) = delete;
  MultiQueue& operator=(const MultiQueue&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  virtual ~MultiQueue();

  // Queue handle to an entry stored in the cache.
  struct Handle {};

  // used by Table::Open
  // insert a handle contains filter into multi-queue
  // deleter will be called when handle is freed
  // filter will be loading in other thread
  virtual Handle* Insert(const Slice& key, FilterBlockReader* reader,
                         void (*deleter)(const Slice& key,
                                         FilterBlockReader* value)) = 0;

  virtual void DoAdjustment(Handle* handle, SequenceNumber sn) = 0;

  virtual bool UpdateHandle(Handle* handle, uint64_t block_offset,
                            const Slice& key) = 0;

  // found a handle save in multi queue
  // key : [filter.filter name][table file id]
  virtual Handle* Lookup(const Slice& key) = 0;

  // Used by Table::Open
  // re init filter which be released
  virtual void GoBackToInitFilter(Handle* handle, RandomAccessFile* file) = 0;

  // Used by Table::KeyMayMatch
  // Try to adjust
  virtual void UpdateHandle(Handle* handle, const Slice& key) = 0;

  // Used by Table::~Table
  // evict all filter when table is freed
  virtual void Release(Handle* handle) = 0;

  // Used by RemoveObsoleteFiles after compaction
  // free handle and filterblock reader saved in multi queue
  virtual void Erase(const std::string & key) = 0;

  // get filterblock reader from handle
  // not need lock
  virtual FilterBlockReader* Value(Handle* handle) = 0;

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  virtual size_t TotalCharge() const = 0;

  // set a logger to record adjustment information
  // in db/LOG
  virtual void SetLogger(Logger* logger) = 0;

  virtual void Lock() = 0;

  virtual void UnLock() = 0;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_MULTI_QUEUE_H_