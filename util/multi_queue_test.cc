// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/multi_queue.h"

#include <vector>

#include "file_impl.h"
#include "gtest/gtest.h"

namespace leveldb {
class MultiQueueTest : public testing::Test {
 public:
  MultiQueueTest() {
    multi_queue_ = NewMultiQueue();
    policy_ = NewBloomFilterPolicy(10);
    internal_policy_ = new InternalFilterPolicy(policy_);
  }

  ~MultiQueueTest() {
    delete multi_queue_;
    delete internal_policy_;
    delete policy_;
    for (int i = 0; i < file_impl_.size(); i++) {
      delete file_impl_[i];
    }
  }

  FilterBlockReader* NewReader() {
    if(filters_number <= 0) return nullptr;
    FilterBlockBuilder builder(internal_policy_);
    builder.StartBlock(100);
    InternalKey key("foo", 10, kTypeValue);
    builder.AddKey(key.Encode());

    FileImpl* file = new FileImpl();
    BlockHandle handle;
    const std::vector<std::string>& filters = builder.ReturnFilters();
    file->WriteRawFilters(filters, &handle);

    Slice block = builder.Finish(handle);

    char* filter_meta = new char [block.size()];
    memcpy(filter_meta, block.data(), block.size());
    Slice filter_meta_data(filter_meta, block.size());

    StringSource* source = file->GetSource();
    file_impl_.push_back(file);
    FilterBlockReader* reader = new FilterBlockReader(internal_policy_, filter_meta_data, source);
    return reader;
  }

  static void Deleter(const Slice& key, FilterBlockReader* reader) {
    delete reader;
  }

  MultiQueue::Handle* Insert(const Slice& key) {
    FilterBlockReader* reader = NewReader();
    MultiQueue::Handle* handle = multi_queue_->Insert(key, reader, MultiQueueTest::Deleter);
    return handle;
  }

  MultiQueue::Handle* Lookup(const Slice& key) const {
    return multi_queue_->Lookup(key);
  }

  void Release(MultiQueue::Handle* handle) const { multi_queue_->Release(handle); }

  void Erase(const Slice& key) const { multi_queue_->Erase(key); }

  FilterBlockReader* Value(MultiQueue::Handle* handle) const {
    return multi_queue_->Value(handle);
  }

  size_t TotalCharge() const { return multi_queue_->TotalCharge(); }

  bool KeyMayMatchSearchExisted(MultiQueue::Handle* handle,
                                SequenceNumber sn = 10) const {
    if(handle == nullptr) return true;
    InternalKey key("foo", sn, kTypeValue);
    return multi_queue_->UpdateHandle(handle, 100, key.Encode());
  }

  bool KeyMayMatchSearchNotExisted(MultiQueue::Handle* handle) const {
    if(handle == nullptr) return true;
    InternalKey key("key", 10, kTypeValue);
    return multi_queue_->UpdateHandle(handle, 100, key.Encode());
  }

  void GoBackToInitFilter(MultiQueue::Handle* handle) const {
    multi_queue_->GoBackToInitFilter(handle, nullptr);
  }

 private:
  MultiQueue* multi_queue_;
  const FilterPolicy* internal_policy_;
  const FilterPolicy* policy_;
  std::vector<FileImpl*> file_impl_;
};

TEST_F(MultiQueueTest, InsertAndLookup) {
  if(filters_number <= 0) return ;
  // insert kv to cache
  MultiQueue::Handle* insert_handle = Insert("key1");
  ASSERT_NE(insert_handle, nullptr);

  // lookup kv from cache
  MultiQueue::Handle* lookup_handle = Lookup("key1");
  ASSERT_NE(lookup_handle, nullptr);
  ASSERT_EQ(lookup_handle, insert_handle);
  ASSERT_TRUE(KeyMayMatchSearchExisted(lookup_handle));
  ASSERT_FALSE(KeyMayMatchSearchNotExisted(lookup_handle));

  MultiQueue::Handle* lookup_handle_no_found = Lookup("key2");
  ASSERT_EQ(lookup_handle_no_found, nullptr);
  ASSERT_TRUE(KeyMayMatchSearchExisted(lookup_handle_no_found));
  ASSERT_TRUE(KeyMayMatchSearchNotExisted(lookup_handle_no_found));

  FilterBlockReader* reader = Value(lookup_handle);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(KeyMayMatchSearchExisted(lookup_handle));
}

TEST_F(MultiQueueTest, InsertAndErase) {
  if(filters_number <= 0) return ;
  MultiQueue::Handle* insert_handle = Insert("key1");
  ASSERT_NE(insert_handle, nullptr);

  ASSERT_EQ(Value(insert_handle)->FilterUnitsNumber(), loaded_filters_number);

  // erase from cache
  Release(insert_handle);

  // can not be found in cache
  MultiQueue::Handle* lookup_handle = Lookup("key1");
  ASSERT_NE(lookup_handle, nullptr);

  ASSERT_EQ(Value(lookup_handle)->FilterUnitsNumber(), 0);

  Erase("key1");
  lookup_handle = Lookup("key1");
  ASSERT_EQ(lookup_handle, nullptr);
}

TEST_F(MultiQueueTest, TotalCharge) {
  if(filters_number <= 0) return ;
  MultiQueue::Handle* insert_handle = Insert("key1");
  ASSERT_NE(insert_handle, nullptr);

  FilterBlockReader* reader = Value(insert_handle);
  ASSERT_EQ(TotalCharge(), reader->Size());

  Release(insert_handle);  // erase from cache, but still in memory
  ASSERT_EQ(TotalCharge(), 0);

  GoBackToInitFilter(insert_handle);
  ASSERT_EQ(TotalCharge(), reader->Size());

  Erase("key1");
  ASSERT_EQ(TotalCharge(), 0);
}

TEST_F(MultiQueueTest, GoBackToInitFilter) {
  if(filters_number <= 0) return ;
  // insert kv to cache
  MultiQueue::Handle* insert_handle = Insert("key1");
  ASSERT_NE(insert_handle, nullptr);
  ASSERT_EQ(Value(insert_handle)->FilterUnitsNumber(), loaded_filters_number);

  Release(insert_handle);
  ASSERT_NE(insert_handle, nullptr);
  ASSERT_EQ(Value(insert_handle)->FilterUnitsNumber(), 0);

  GoBackToInitFilter(insert_handle);
  ASSERT_NE(insert_handle, nullptr);
  ASSERT_EQ(Value(insert_handle)->FilterUnitsNumber(), loaded_filters_number);
}


TEST_F(MultiQueueTest, Adjustment) {
  if(filters_number <= 0) return ;
  MultiQueue::Handle* cold_handle = Insert("cold");
  MultiQueue::Handle* hot_handle  = Insert("hot");
  ASSERT_NE(cold_handle, nullptr);
  ASSERT_NE(hot_handle,  nullptr);

  // init filter number may not be 2
  while(Value(cold_handle)->FilterUnitsNumber() < 2){
    if(!Value(cold_handle)->LoadFilter().ok()){
      break;
    }
  }

  while(Value(hot_handle)->FilterUnitsNumber() < 2){
    if(!Value(hot_handle)->LoadFilter().ok()){
      break; // if filter units number less than 2, break it
    }
  }

  for(int i = 0; i < 1000; i++){
    KeyMayMatchSearchExisted(cold_handle);
  }

  for(int i = 0; i < 1000000; i++){
    KeyMayMatchSearchExisted(hot_handle, i + 10 + life_time);
  }

  ASSERT_EQ(Value(hot_handle)->AccessTime(), 1000000);
  ASSERT_EQ(Value(cold_handle)->AccessTime(), 1000);

  if(filters_number >= 3) {
    // cold handle evict one filter
    // hot handle load one filter
    ASSERT_EQ(Value(cold_handle)->FilterUnitsNumber(), 1);
    ASSERT_EQ(Value(hot_handle)->FilterUnitsNumber(), 3);
  }
}
}  // namespace leveldb
