// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "db/dbformat.h"
#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {
  filter_units_.resize(filters_number);
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

const std::vector<std::string>& FilterBlockBuilder::ReturnFilters() {
  if (!start_.empty()) {
    GenerateFilter();
    // generate limit offset for last filter
    // array offset is last filter's limit in leveldb
    // but we remove it
    filter_offsets_.push_back(filter_units_[0].size());
  }

  // const & to reduce vector copy overhead
  return filter_units_;
}

/*
 * filter offset <--- unint32_t for every offset
 * offset        <--- unint64_t  first filter offset in disk
 * size          <--- unint32_t  one filter size
 * loaded        <--- unint32_t  the number of filter to load when FilterBlockReader is created
 * number        <--- unint32_t  all filters number
 * baselg        <--- char
 */
Slice FilterBlockBuilder::Finish(const BlockHandle& handle) {
  // Append array of per-filter offsets
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  PutFixed64(&result_, handle.offset());
  PutFixed32(&result_, handle.size());
  PutFixed32(&result_, loaded_filters_number);
  PutFixed32(&result_, filters_number);

  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result

  // to reduce copy overhead, string can not be modified
  // similar to const string&
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    // all filters has same structure, so pick up filter 0
    filter_offsets_.push_back(filter_units_[0].size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(filter_units_[0].size());
  for(int i = 0; i < filters_number; i++){
    // generate different bitmap for different filter units
    policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &filter_units_[i], i);
  }

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents, RandomAccessFile* file)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0),
      file_(file), heap_allocated_(false), access_time_(0), sequence_(0) {
  size_t n = contents.size();
  if (n < 21) return;  // 1 byte for base_lg_ and 21 for start of others

  /*
 * meta data for filter units' bitmaps layout:
 * filter offset  <--- data + 0
 * offset         <--- data + n - 21  | 8Byte 4Byte can only index 4GB disk offset
 * size           <--- data + n - 13  | 4Byte
 * loaded         <--- data + n - 9   | 4Byte
 * number         <--- data + n - 5   | 4Byte
 * base lg        <--- data + n - 1   | 1Byte
   */
  base_lg_ = contents[n - 1];
  data_ = contents.data();
  all_units_number_ = DecodeFixed32(data_ + n - 5);
  if(all_units_number_ < 0) return;

  init_units_number_ = DecodeFixed32(data_ + n - 9);

  if(init_units_number_  < 0 && init_units_number_ > all_units_number_) return;

  disk_size_ = DecodeFixed32(data_ + n - 13);
  disk_offset_ = DecodeFixed64(data_ + n - 21);
  offset_ =  contents.data();

  num_ = (n - 21) / 4;

  //todo desgin new error catch code
  Status status;
  for(int i = 0; i < init_units_number_; i++) {
    status = LoadFilter();
    if(!status.ok()){
      return ;
    }
  }
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  ParsedInternalKey parsedInternalKey;
  if(ParseInternalKey(key, &parsedInternalKey)) {
    sequence_ = parsedInternalKey.sequence;
  }
  access_time_++;

  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    // limit should not larger than filter size
    if (start <= limit && limit <= static_cast<size_t>(disk_size_)) {
      Slice filter;
      // every filter return true, return true
      // at least one filter return false, return false
      // bloom filter has no false negative rate, but has false positive rate
      for(int i = 0; i < filter_units.size(); i++){
        filter = Slice(filter_units[i] + start, limit - start);
        if(!policy_->KeyMayMatch(key, filter, i)){
          return false;
        }
      }
      return true;
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

/*
 * filters in disk layout:
 * trailer type(kNoCompression) CRC <--- disk_offset
 * filter
 * trailer type(kNoCompression) CRC <--- disk_offset + (disk_size + kBlockTrailerSize)
 * filter
 * .........
 * trailer type(kNoCompression) CRC
 * filter
 */
Status FilterBlockReader::LoadFilter() {
  uint64_t units_index  = filter_units.size();
  if(units_index >= all_units_number_)
    return Status::Corruption("all filter units were loaded");

  ReadOptions readOptions;
  readOptions.verify_checksums = true;

  BlockHandle handle;
  BlockContents contents;

  // every filter has same size: disk_size_ + kBlockTrailerSize
  uint64_t offset = disk_offset_ + (disk_size_ + kBlockTrailerSize) * units_index;
  handle.set_offset(offset);
  handle.set_size(disk_size_);

  Status s = ReadBlock(file_, readOptions, handle, &contents);

  if(!s.ok()) return s;

  // if heap_allocated, delete by FilterBlockReader
  // if not, delete by mmap
  heap_allocated_ = contents.heap_allocated;
  filter_units.push_back(contents.data.data());
  return s;
}

Status FilterBlockReader::EvictFilter() {
  if(filter_units.empty())
    return Status::Corruption("there is no filter can be  evicted");

  uint32_t size = filter_units.size();
  // load from left to right
  // evict from right to left
  const char* data = filter_units[size - 1];
  delete[] data;
  filter_units.pop_back();
  return Status::OK();
}

FilterBlockReader::~FilterBlockReader() {
  if(heap_allocated_){
    for(const char * filter_unit : filter_units){
      delete[] filter_unit;
    }
  }

  delete[] data_;
}
}  // namespace leveldb
