// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"
#include "leveldb/filter_policy.h"
#include "util/file_impl.h"
#include "gtest/gtest.h"

namespace leveldb {
// For testing: emit an array with one hash value per key
class TestHashFilter : public FilterPolicy {
 public:
  const char* Name() const override { return "TestHashFilter"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst,
                    int index) const override {
    for (int i = 0; i < n; i++) {
      uint32_t h = Hash(keys[i].data(), keys[i].size(), index);
      PutFixed32(dst, h);
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& filter,
                   int index) const override {
    uint32_t h = Hash(key.data(), key.size(), index);
    for (size_t i = 0; i + 4 <= filter.size(); i += 4) {
      if (h == DecodeFixed32(filter.data() + i)) {
        return true;
      }
    }
    return false;
  }

  // TestHashFilter save completely data, no false positive rate
  double FalsePositiveRate() const override { return 0; }
};

class FilterBlockTest : public testing::Test {
 public:
  Slice FinishBuilder(FilterBlockBuilder& builder) {
    BlockHandle handle;
    const std::vector<std::string>& filters = builder.ReturnFilters();
    file.WriteRawFilters(filters, &handle);
    Slice block = builder.Finish(handle);
    return block;
  }

  FilterBlockReader* GetReader(Slice& block,
                               const FilterPolicy* policy = nullptr) {
    char* filter_meta = new char[block.size()];
    memcpy(filter_meta, block.data(), block.size());
    Slice filter_meta_data(filter_meta, block.size());
    StringSource* source = file.GetSource();
    FilterBlockReader* reader = new FilterBlockReader(
        policy == nullptr ? &policy_ : policy, filter_meta_data, source);
    reader->InitLoadFilter();
    return reader;
  }

  FilterBlockReader* GetReader(FilterBlockBuilder& builder,
                               const FilterPolicy* policy = nullptr) {
    Slice block = FinishBuilder(builder);
    return GetReader(block, policy);
  }

  TestHashFilter policy_;
  FileImpl file;
};

TEST_F(FilterBlockTest, EmptyBuilder) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);

  Slice block = FinishBuilder(builder);

  ASSERT_EQ(
      "\\x00\\x00\\x00\\x00"                      // bitmap len
      "\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00"  // offset
      "\\x00\\x00\\x00\\x00"                      // size
      "\\x0" +
          std::to_string(loaded_filters_number) +
          "\\x00\\x00\\x00"  // loaded
          "\\x0" +
          std::to_string(filters_number) +
          "\\x00\\x00\\x00"  // number
          "\\x0b",           // baselg
      EscapeString(block));

  FilterBlockReader* reader = GetReader(block);
  ASSERT_TRUE(reader->KeyMayMatch(0, "foo"));
  ASSERT_TRUE(reader->KeyMayMatch(100000, "foo"));

  delete reader;
}

TEST_F(FilterBlockTest, SingleChunk) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);
  builder.StartBlock(100);
  builder.AddKey("foo");
  builder.AddKey("bar");
  builder.AddKey("box");
  builder.StartBlock(200);
  builder.AddKey("box");
  builder.StartBlock(300);
  builder.AddKey("hello");

  Slice block = FinishBuilder(builder);

  std::string escapestring = EscapeString(block);
  // remove filter's bitmap
  escapestring = escapestring.substr(escapestring.size() - 25 * 4, 25 * 4);

  ASSERT_EQ(
      "\\x14\\x00\\x00\\x00"  // bitmap len(20 = 4Byte * 5 key)
      "\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00"  // bitmap offset(0)
      // bitmap size in disk, as same as bitmap len
      "\\x14\\x00\\x00\\x00"
      "\\x0" +
          std::to_string(loaded_filters_number) +
          "\\x00\\x00\\x00"
          "\\x0" +
          std::to_string(filters_number) +
          "\\x00\\x00\\x00"
          "\\x0b",
      escapestring);

  FilterBlockReader* reader = GetReader(block);

  ASSERT_TRUE(reader->KeyMayMatch(100, "foo"));
  ASSERT_TRUE(reader->KeyMayMatch(100, "bar"));
  ASSERT_TRUE(reader->KeyMayMatch(100, "box"));
  ASSERT_TRUE(reader->KeyMayMatch(100, "hello"));
  ASSERT_TRUE(reader->KeyMayMatch(100, "foo"));
  ASSERT_TRUE(!reader->KeyMayMatch(100, "missing"));
  ASSERT_TRUE(!reader->KeyMayMatch(100, "other"));

  delete reader;
}

TEST_F(FilterBlockTest, MultiChunk) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);

  // First filter
  builder.StartBlock(0);
  builder.AddKey("foo");
  builder.StartBlock(2000);
  builder.AddKey("bar");

  // Second filter
  builder.StartBlock(3100);
  builder.AddKey("box");

  // Third filter is empty

  // Last filter
  builder.StartBlock(9000);
  builder.AddKey("box");
  builder.AddKey("hello");

  // create reader
  FilterBlockReader* reader = GetReader(builder);

  // Check first filter
  ASSERT_TRUE(reader->KeyMayMatch(0, "foo"));
  ASSERT_TRUE(reader->KeyMayMatch(2000, "bar"));
  ASSERT_TRUE(!reader->KeyMayMatch(0, "box"));
  ASSERT_TRUE(!reader->KeyMayMatch(0, "hello"));

  // Check second filter
  ASSERT_TRUE(reader->KeyMayMatch(3100, "box"));
  ASSERT_TRUE(!reader->KeyMayMatch(3100, "foo"));
  ASSERT_TRUE(!reader->KeyMayMatch(3100, "bar"));
  ASSERT_TRUE(!reader->KeyMayMatch(3100, "hello"));

  // Check third filter (empty)
  ASSERT_TRUE(!reader->KeyMayMatch(4100, "foo"));
  ASSERT_TRUE(!reader->KeyMayMatch(4100, "bar"));
  ASSERT_TRUE(!reader->KeyMayMatch(4100, "box"));
  ASSERT_TRUE(!reader->KeyMayMatch(4100, "hello"));

  // Check last filter
  ASSERT_TRUE(reader->KeyMayMatch(9000, "box"));
  ASSERT_TRUE(reader->KeyMayMatch(9000, "hello"));
  ASSERT_TRUE(!reader->KeyMayMatch(9000, "foo"));
  ASSERT_TRUE(!reader->KeyMayMatch(9000, "bar"));

  delete reader;
}

TEST_F(FilterBlockTest, LoadAndExcit) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);

  // First filter
  builder.StartBlock(0);
  builder.AddKey("foo");
  builder.StartBlock(2000);
  builder.AddKey("bar");

  // Second filter
  builder.StartBlock(3100);
  builder.AddKey("box");

  // Third filter is empty

  // Last filter
  builder.StartBlock(9000);
  builder.AddKey("box");
  builder.AddKey("hello");

  FilterBlockReader* reader = GetReader(builder);
  // todo can automatically adapt to different parameters
  for (int i = loaded_filters_number; i > 0; i--) {
    ASSERT_EQ(reader->FilterUnitsNumber(), i);
    ASSERT_TRUE(reader->EvictFilter().ok());
  }

  ASSERT_FALSE(reader->EvictFilter().ok());

  for (int i = 0; i < filters_number; i++) {
    ASSERT_EQ(reader->FilterUnitsNumber(), i);
    ASSERT_TRUE(reader->LoadFilter().ok());
  }

  ASSERT_FALSE(reader->LoadFilter().ok());
  delete reader;
}

TEST_F(FilterBlockTest, Hotness) {
  if (filters_number <= 0) return;
  // to support internal key
  InternalFilterPolicy policy(&policy_);
  FilterBlockBuilder builder(&policy);

  // First filter
  builder.StartBlock(0);
  ParsedInternalKey add_key("foo", 1, kTypeValue);
  std::string add_result;
  AppendInternalKey(&add_result, add_key);
  builder.AddKey(add_result);

  FilterBlockReader* reader = GetReader(builder, &policy);

  // check
  for (uint64_t sn = 1; sn < 30000; sn++) {
    ParsedInternalKey check_key("foo", sn, kTypeValue);
    std::string check_result;
    AppendInternalKey(&check_result, check_key);
    // sequence number is sn
    ASSERT_TRUE(reader->KeyMayMatch(0, check_result));
    reader->UpdateState(sn);
    ASSERT_EQ(reader->AccessTime(), sn);

    // reader died in sn + 30000
    ASSERT_FALSE(reader->IsCold(30000 + sn - 1));
    ASSERT_TRUE(reader->IsCold(30000 + sn));
  }

  delete reader;
}

TEST_F(FilterBlockTest, Size) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);
  builder.StartBlock(100);
  builder.AddKey("foo");
  builder.AddKey("bar");
  builder.AddKey("box");
  builder.StartBlock(200);
  builder.AddKey("box");
  builder.StartBlock(300);
  builder.AddKey("hello");

  FileImpl file;
  BlockHandle handle;
  const std::vector<std::string>& filters = builder.ReturnFilters();
  file.WriteRawFilters(filters, &handle);

  size_t bitmap_size = handle.size();
  Slice block = builder.Finish(handle);

  char* filter_meta = new char[block.size()];
  memcpy(filter_meta, block.data(), block.size());
  Slice filter_meta_data(filter_meta, block.size());

  StringSource* source = file.GetSource();
  FilterBlockReader reader(&policy_, filter_meta_data, source);
  reader.InitLoadFilter();

  // evict all filter units
  while (reader.EvictFilter().ok()) {
  }
  ASSERT_EQ(reader.FilterUnitsNumber(), 0);
  ASSERT_EQ(reader.Size(), 0);

  // load filter units one by one
  // check memory overhead
  int filter_unit_number = 1;
  while (reader.LoadFilter().ok()) {
    ASSERT_EQ(reader.FilterUnitsNumber(), filter_unit_number);
    ASSERT_EQ(reader.Size(), bitmap_size * filter_unit_number);
    filter_unit_number++;
  }
}

TEST_F(FilterBlockTest, IOs) {
  if (filters_number <= 0) return;
  const FilterPolicy* policy = NewBloomFilterPolicy(10);
  FilterBlockBuilder builder(policy);
  builder.StartBlock(100);
  builder.AddKey("foo");
  builder.AddKey("bar");
  builder.AddKey("box");
  builder.StartBlock(200);
  builder.AddKey("box");
  builder.StartBlock(300);
  builder.AddKey("hello");

  FilterBlockReader* reader = GetReader(builder, policy);

  int access = 1000;
  for (int i = 0; i < access; i++) {
    ASSERT_TRUE(reader->KeyMayMatch(100, "foo"));
    reader->UpdateState(0);
  }

  double false_positive_rate = pow(0.6185, 10);

  ASSERT_EQ(reader->AccessTime(), access);
  // get ios by myself
  ASSERT_EQ(reader->IOs(),
            pow(false_positive_rate, loaded_filters_number) * access);

  if (loaded_filters_number < filters_number) {
    ASSERT_EQ(reader->LoadIOs(),
              pow(false_positive_rate, loaded_filters_number + 1) * access);
  }

  if (loaded_filters_number > 1) {
    ASSERT_EQ(reader->EvictIOs(),
              pow(false_positive_rate, loaded_filters_number - 1) * access);
  }

  delete policy;
  delete reader;
}

TEST_F(FilterBlockTest, GoBackToInitFilter) {
  if (filters_number <= 0) return;
  FilterBlockBuilder builder(&policy_);

  // First filter
  builder.StartBlock(0);
  builder.AddKey("foo");
  builder.StartBlock(2000);
  builder.AddKey("bar");

  // Second filter
  builder.StartBlock(3100);
  builder.AddKey("box");

  // Third filter is empty

  // Last filter
  builder.StartBlock(9000);
  builder.AddKey("box");
  builder.AddKey("hello");

  FilterBlockReader* reader = GetReader(builder);

  ASSERT_TRUE(reader->GoBackToInitFilter(nullptr).ok());
  ASSERT_EQ(reader->FilterUnitsNumber(), reader->LoadFilterNumber());

  if (reader->CanBeEvict()) {
    ASSERT_TRUE(reader->EvictFilter().ok());
  }

  ASSERT_TRUE(reader->GoBackToInitFilter(nullptr).ok());
  ASSERT_EQ(reader->FilterUnitsNumber(), reader->LoadFilterNumber());

  if (reader->CanBeLoaded()) {
    ASSERT_TRUE(reader->LoadFilter().ok());
  }

  ASSERT_TRUE(reader->GoBackToInitFilter(nullptr).ok());
  ASSERT_EQ(reader->FilterUnitsNumber(), reader->LoadFilterNumber());

  delete reader;
}

}  // namespace leveldb
