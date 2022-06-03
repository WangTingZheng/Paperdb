//
// Created by 14037 on 2022/5/25.
//

#ifndef LEVELDB_VLOG_READER_H
#define LEVELDB_VLOG_READER_H

#include <atomic>
#include "leveldb/status.h"
#include "leveldb/env.h"
#include "dbformat.h"

namespace leveldb {
    namespace vlog {
        class VlogReader {
            public:
                explicit VlogReader(const std::string& dbname, uint64_t log_number);

                ~VlogReader();

                // 从文件中读取[offset, size]到value中
                Status Read(uint64_t offset, uint64_t size, Slice *value, char *scratch);

                //
                bool ReadRecord(Slice* record, std::string* scratch);

                // 读取当前位置pos的record的key，以及这一条record的<log_number, offset, size>
                bool ReadMeta(uint64_t *pos, std::string *key, SequenceNumber *sn, std::string *meta);

                // 跳转到offfset处，offset必须大于等0
                Status Jump(uint64_t offset);

            private:
                SequentialFile *file_;
                uint64_t log_number_;
                bool JumpTo(uint64_t offset);

                bool const checksum_;
                char* const backing_store_;
                Slice buffer_;
                bool eof_;  // Last Read() indicated EOF by returning <
                // Reports dropped bytes to the reporter.
                // buffer_ must be updated to remove the dropped bytes prior to invocation.
                static const int kBlockSize = 32768;
        };
    }
} // leveldb

#endif //LEVELDB_VLOG_READER_H
