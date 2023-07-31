//
// Created by WangTingZheng on 2023/6/19.
//

#include "file_impl.h"

#include "util/coding.h"

namespace leveldb {
FileImpl::FileImpl() : write_offset_(0) {
  sink_ = new StringSink();
  source_ = nullptr;
}

void FileImpl::WriteRawFilters(std::vector<std::string> filters,
                               BlockHandle* handle) {
  assert(!filters.empty());
  handle->set_offset(write_offset_);
  handle->set_size(filters[0].size());

  Status status;

  for (int i = 0; i < filters.size(); i++) {
    std::string filter = filters[i];
    Slice filter_slice = Slice(filter);
    status = sink_->Append(filter_slice);
    if (status.ok()) {
      char trailer[kBlockTrailerSize];
      trailer[0] = kNoCompression;
      uint32_t crc = crc32c::Value(filter_slice.data(), filter_slice.size());
      crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
      EncodeFixed32(trailer + 1, crc32c::Mask(crc));
      status = sink_->Append(Slice(trailer, kBlockTrailerSize));
      if (status.ok()) {
        write_offset_ += filter_slice.size() + kBlockTrailerSize;
      }
    }
  }
}

StringSource* FileImpl::GetSource() {
  if (source_ == nullptr) {
    source_ = new StringSource(sink_->contents());
  }
  return source_;
}


FileImpl::~FileImpl() {
  delete sink_;
  delete source_;
}

Status SpecialEnv::NewWritableFile(const std::string& f, WritableFile** r) {
  class DataFile : public WritableFile {
   private:
    SpecialEnv* const env_;
    WritableFile* const base_;
    const std::string fname_;

   public:
    DataFile(SpecialEnv* env, WritableFile* base, const std::string& fname)
        : env_(env), base_(base), fname_(fname) {}

    ~DataFile() { delete base_; }
    Status Append(const Slice& data) {
      if (env_->no_space_.load(std::memory_order_acquire)) {
        // Drop writes on the floor
        return Status::OK();
      } else {
        return base_->Append(data);
      }
    }
    Status Close() {
      Status s = base_->Close();
      if (s.ok() && IsLogFile(fname_) &&
          env_->log_file_close_.load(std::memory_order_acquire)) {
        s = Status::IOError("simulated log file Close error");
      }
      return s;
    }
    Status Flush() { return base_->Flush(); }
    Status Sync() {
      if (env_->data_sync_error_.load(std::memory_order_acquire)) {
        return Status::IOError("simulated data sync error");
      }
      while (env_->delay_data_sync_.load(std::memory_order_acquire)) {
        DelayMilliseconds(100);
      }
      return base_->Sync();
    }
  };
  class ManifestFile : public WritableFile {
   private:
    SpecialEnv* env_;
    WritableFile* base_;

   public:
    ManifestFile(SpecialEnv* env, WritableFile* b) : env_(env), base_(b) {}
    ~ManifestFile() { delete base_; }
    Status Append(const Slice& data) {
      if (env_->manifest_write_error_.load(std::memory_order_acquire)) {
        return Status::IOError("simulated writer error");
      } else {
        return base_->Append(data);
      }
    }
    Status Close() { return base_->Close(); }
    Status Flush() { return base_->Flush(); }
    Status Sync() {
      if (env_->manifest_sync_error_.load(std::memory_order_acquire)) {
        return Status::IOError("simulated sync error");
      } else {
        return base_->Sync();
      }
    }
  };

  if (non_writable_.load(std::memory_order_acquire)) {
    return Status::IOError("simulated write error");
  }

  Status s = target()->NewWritableFile(f, r);
  if (s.ok()) {
    if (IsLdbFile(f) || IsLogFile(f)) {
      *r = new DataFile(this, *r, f);
    } else if (IsManifestFile(f)) {
      *r = new ManifestFile(this, *r);
    }
  }
  return s;
}

Status SpecialEnv::NewRandomAccessFile(const std::string& f, RandomAccessFile** r) {
  class CountingFile : public RandomAccessFile {
   private:
    RandomAccessFile* target_;
    AtomicCounter* counter_;

   public:
    CountingFile(RandomAccessFile* target, AtomicCounter* counter)
        : target_(target), counter_(counter) {}
    ~CountingFile() override { delete target_; }
    Status Read(uint64_t offset, size_t n, Slice* result,
                ReadBuffer* scratch) const override {
      counter_->Increment();
      return target_->Read(offset, n, result, scratch);
    }
  };

  Status s = target()->NewRandomAccessFile(f, r);
  if (s.ok() && count_random_reads_.load(std::memory_order_acquire)) {
    *r = new CountingFile(*r, &random_read_counter_);
  }
  return s;
}

Status SpecialEnv::NewDirectIORandomAccessFile(const std::string& f, RandomAccessFile** r) {
  class DirectIOCountingFile : public RandomAccessFile {
   private:
    RandomAccessFile* target_;
    AtomicCounter* counter_;

   public:
    DirectIOCountingFile(RandomAccessFile* target, AtomicCounter* counter)
        : target_(target), counter_(counter) {}
    ~DirectIOCountingFile() override { delete target_; }
    Status Read(uint64_t offset, size_t n, Slice* result,
                ReadBuffer* scratch) const override {
      counter_->Increment();
      return target_->Read(offset, n, result, scratch);
    }
  };

  Status s = target()->NewDirectIORandomAccessFile(f, r);
  if (s.ok() && count_random_reads_.load(std::memory_order_acquire)) {
    *r = new DirectIOCountingFile(*r, &random_read_counter_);
  }
  return s;
}
};  // namespace leveldb