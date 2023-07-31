// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/multi_queue.h"
#include "leveldb/options.h"

#include "table/block.h"
#include "table/two_level_iterator.h"
#include "util/mq_schedule.h"

namespace leveldb {
struct Table::Rep {
  ~Rep() {
    delete index_block;
    delete reader;
    MultiQueue* multi_queue = options.multi_queue;
    if (multi_queue && handle) {
      // release for this table
      // save sequence and hotness in multi queue
      // just evict all filter units
      multi_queue->Release(handle);
      handle = nullptr;
    }
  }

  Options options;
  Status status;
  RandomAccessFile* file;
  uint64_t block_cache_id;
  uint64_t table_id;
  Footer footer;
  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  Block* index_block;
  MultiQueue::Handle* handle;
  FilterBlockReader* reader;
  std::string multi_cache_key;
};

std::string Table::ParseHandleKey(const Options& options,
                                  uint64_t file_id) {
  std::string key;
  assert(options.filter_policy);
  key = "filter.";
  key.append(options.filter_policy->Name());
  char cache_key_buffer[8];
  EncodeFixed64(cache_key_buffer, file_id);
  key.append(cache_key_buffer, 8);
  return key;
}

void Table::ParseHandleKey() {
  std::string key;
  Options options = rep_->options;
  if(options.filter_policy) {
    // todo: use std::move?
    rep_->multi_cache_key = ParseHandleKey(rep_->options, rep_->table_id);
  }
}

Status Table::Open(const Options& options, RandomAccessFile* file,
                   uint64_t size, Table** table, uint64_t table_id) {
  *table = nullptr;                             //every table has unique file id
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Read the index block
  BlockContents index_block_contents;
  ReadOptions opt;
  if (options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    Block* index_block = new Block(index_block_contents);
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    rep->block_cache_id =
        (options.block_cache ? options.block_cache->NewId() : 0);
    rep->table_id = table_id;
    rep->handle = nullptr;
    rep->reader = nullptr;
    rep->footer = footer;
    *table = new Table(rep);
    (*table)->ParseHandleKey();
    (*table)->ReadMeta();
  }

  return s;
}

// Read filter block from disk, if needed. ReadMeta maybe called
// by ReadFilter, so, footer need saved in rep
FilterBlockReader* Table::ReadFilter() {
  if (rep_->options.filter_policy == nullptr) {
    return nullptr;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, rep_->footer.metaindex_handle(), &contents)
           .ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return nullptr;
  }
  FilterBlockReader* reader = nullptr;
  Block* meta = new Block(contents);

  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == Slice(key)) {
    Slice filter_meta = iter->value();
    size_t filter_meta_size = filter_meta.size();
    assert(filter_meta_size >= 21);
    // meta index block will be deleted later
    // malloc maybe segmentation fault
    char* filter_meta_contents = new char[filter_meta_size];
    memcpy(filter_meta_contents, filter_meta.data(), filter_meta_size);
    // The length must be passed in, if only a char* pointer is passed in,
    // the length will be taken with strlen() and the string will
    // be truncated by \0, and thus an error will occur
    Slice filter_meta_data(filter_meta_contents, filter_meta_size);
    reader = new FilterBlockReader(rep_->options.filter_policy,
                                   filter_meta_data, rep_->file);
  }
  delete iter;
  delete meta;

  return reader;
}

static void DeleteCacheFilter(const Slice& key, FilterBlockReader* value) {
  delete value;
}

// get FilterBlockReader and saved in rep_->filter, we should return
// cache handle and reader, so the pointer of reader passed in ReadFilter
void Table::ReadMeta() {
  MultiQueue* multi_queue = rep_->options.multi_queue;
  if (rep_->options.filter_policy == nullptr || rep_->reader != nullptr
      || rep_->handle != nullptr) {
    return;
  }

  if (multi_queue == nullptr) {
    if (rep_->reader == nullptr) {
      rep_->reader = ReadFilter();
      if(rep_->reader != nullptr) {
        rep_->reader->InitLoadFilter();
      }
    }
    return;
  }


  // Get filter block from cache, or read from disk and insert
  std::string filter_key = rep_->multi_cache_key;
  Slice key(filter_key.data(), filter_key.size());

  MultiQueue::Handle* handle = multi_queue->Lookup(key);
  if (handle == nullptr) { //not in multi queue, insert
    FilterBlockReader* reader = ReadFilter();
    if (reader != nullptr) {
      handle = multi_queue->Insert(key, reader, &DeleteCacheFilter);
    }
  } else{ // in multi queue, load filter
    // check filter unit number
    // prev file object will be free, update new file object
    multi_queue->GoBackToInitFilter(handle, rep_->file);
  }

  rep_->handle = handle;
}

Table::~Table() { delete rep_; }

static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}

static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    // if cache_data_block is false, read datablock from disk everytime
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->block_cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(key, block, block->size(),
                                               &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == nullptr) {
      iter->RegisterCleanup(&DeleteBlock, block, nullptr);
    } else {
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

bool Table::MultiQueueKeyMayMatch(uint64_t block_offset, const Slice& key) {
  MultiQueue* multi_queue = rep_->options.multi_queue;
  if(multi_queue && rep_->handle) {
    return multi_queue->UpdateHandle(rep_->handle, block_offset, key);
  }
  return true;
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&,
                                                const Slice&)) {
  Status s;
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
    Slice handle_value = iiter->value();
    BlockHandle handle;
    bool is_decode_ok = handle.DecodeFrom(&handle_value).ok();
    if (rep_->reader && is_decode_ok &&
        !rep_->reader->KeyMayMatch(handle.offset(), k)) {
      // Not found
    } else if (rep_->handle != nullptr && is_decode_ok &&
               !MultiQueueKeyMayMatch(handle.offset(), k)) {
      // Not found
    } else {
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        (*handle_result)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
