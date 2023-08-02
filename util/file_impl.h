//
// Created by WangTingZheng on 2023/6/19.
// Just wrapper for filters writing, for unit test
// StringSink, StringSource: save file in memory
//

#ifndef LEVELDB_FILE_IMPL_H
#define LEVELDB_FILE_IMPL_H

#include <atomic>

#include "leveldb/env.h"
#include "leveldb/status.h"

#include "port/thread_annotations.h"
#include "table/format.h"
#include "util/crc32c.h"

#include "mutexlock.h"

namespace leveldb {

class StringSink : public WritableFile {
 public:
  ~StringSink() override = default;

  const std::string& contents() const;

  Status Close() override;
  Status Flush() override;
  Status Sync() override;

  Status Append(const Slice& data) override;

 private:
  std::string contents_;
};

class StringSource : public RandomAccessFile {
 public:
  StringSource(const Slice& contents);
  ~StringSource() override = default;
  uint64_t Size() const;
  Status Read(uint64_t offset, size_t n, Slice* result,
              ReadBuffer* scratch) const override;
 private:
  std::string contents_;
};

class FileImpl {
 public:
  FileImpl();
  void WriteRawFilters(std::vector<std::string> filters, BlockHandle* handle);
  StringSource* GetSource();
  ~FileImpl();

 private:
  StringSink* sink_;
  StringSource* source_;
  uint64_t write_offset_;
};

class AtomicCounter{
 public:
  AtomicCounter();
  void Increment();
  void IncrementBy(int count);
  int Read();
  void Reset();
 private:
  port::Mutex mu_;
  int count_ GUARDED_BY(mu_);
};

namespace {
void DelayMilliseconds(int millis) {
  Env::Default()->SleepForMicroseconds(millis * 1000);
}

bool IsLdbFile(const std::string& f) {
  return strstr(f.c_str(), ".ldb") != nullptr;
}

bool IsLogFile(const std::string& f) {
  return strstr(f.c_str(), ".log") != nullptr;
}

bool IsManifestFile(const std::string& f) {
  return strstr(f.c_str(), "MANIFEST") != nullptr;
}
}  // namespace

// Special Env used to delay background operations.
class SpecialEnv : public EnvWrapper {
 public:
  // For historical reasons, the std::atomic<> fields below are currently
  // accessed via acquired loads and release stores. We should switch
  // to plain load(), store() calls that provide sequential consistency.

  // sstable/log Sync() calls are blocked while this pointer is non-null.
  std::atomic<bool> delay_data_sync_;

  // sstable/log Sync() calls return an error.
  std::atomic<bool> data_sync_error_;

  // Simulate no-space errors while this pointer is non-null.
  std::atomic<bool> no_space_;

  // Simulate non-writable file system while this pointer is non-null.
  std::atomic<bool> non_writable_;

  // Force sync of manifest files to fail while this pointer is non-null.
  std::atomic<bool> manifest_sync_error_;

  // Force write to manifest files to fail while this pointer is non-null.
  std::atomic<bool> manifest_write_error_;

  // Force log file close to fail while this bool is true.
  std::atomic<bool> log_file_close_;

  std::atomic<bool> count_random_reads_;
  AtomicCounter random_read_counter_;

  explicit SpecialEnv(Env* base)
      : EnvWrapper(base),
        delay_data_sync_(false),
        data_sync_error_(false),
        no_space_(false),
        non_writable_(false),
        manifest_sync_error_(false),
        manifest_write_error_(false),
        log_file_close_(false),
        count_random_reads_(false) {}
  Status NewWritableFile(const std::string& f, WritableFile** r);

  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r);

  Status NewDirectIORandomAccessFile(const std::string& f, RandomAccessFile** r);
};

}  // namespace leveldb

#endif  // LEVELDB_FILE_IMPL_H
