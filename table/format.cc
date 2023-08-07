// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include "leveldb/env.h"
#include "leveldb/options.h"
#include "port/port.h"
#include "table/block.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    return Status::Corruption("bad block handle");
  }
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;  // Disable unused variable warning.
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < kEncodedLength) {
    return Status::Corruption("not an sstable (footer too short)");
  }

  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }

  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result) {
  result->data = Slice();
  result->read_buffer = nullptr;
  result->cachale = false;

  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  size_t n = static_cast<size_t>(handle.size());
  ReadBuffer read_buffer;
  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, &read_buffer);
  if (!s.ok()) {
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  // Check the crc of the type and the block contents
  const char* data = contents.data();  // Pointer to where Read put the data
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }

  switch (data[n]) {
    case kNoCompression:
      result->data = Slice(data, n);
      // mmap's ptr is null, no need for cache
      // TODO add cache unit test?
      result->cachale = read_buffer.PtrIsNotNull();
      // use move semantics to move buffer
      result->read_buffer = new ReadBuffer(std::move(read_buffer));

      // Ok
      break; //TODO test compression?
    case kSnappyCompression: {
      size_t ulength = 0;
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        return Status::Corruption("corrupted snappy compressed block length");
      }
      char* ubuf = (char *)malloc(sizeof(char) * ulength);
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        free(ubuf);
        return Status::Corruption("corrupted snappy compressed block contents");
      }
      //data will be freed when read_buffer object is gone
      result->data = Slice(ubuf, ulength);
      result->read_buffer =  new ReadBuffer(ubuf, /*aligned=*/false);
      break;
    }
    case kZstdCompression: {
      size_t ulength = 0;
      if (!port::Zstd_GetUncompressedLength(data, n, &ulength)) {
        return Status::Corruption("corrupted zstd compressed block length");
      }
      char* ubuf = (char *)malloc(sizeof(char) * ulength);
      if (!port::Zstd_Uncompress(data, n, ubuf)) {
        free(ubuf);
        return Status::Corruption("corrupted zstd compressed block contents");
      }
      result->data = Slice(ubuf, ulength);
      result->read_buffer = new ReadBuffer(ubuf, /*aligned=*/false);
      break;
    }
    default:
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

}  // namespace leveldb
