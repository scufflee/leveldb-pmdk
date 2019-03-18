// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

// temp
#include <chrono>
#include <iostream>

namespace leveldb {

/* PROGRESS: Write file based on pmem */
// /*
Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta) {
  SSTMakerType sst_type = options.sst_type;
  // std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    // TODO: Don't create file
    /*
    WritableFile* file;
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->key());
*/

    if (sst_type == kFileDescriptorSST) {

      WritableFile* file;
      s = env->NewWritableFile(fname, &file);
      if (!s.ok()) {
        return s;
      }
      TableBuilder* builder = new TableBuilder(options, file);
      meta->smallest.DecodeFrom(iter->key());

      int i = 0;
      for (; iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        meta->largest.DecodeFrom(key);
        builder->Add(key, iter->value());
        // 24, 100
        // printf("'%s'-'%s', '%d' '%d'\n", key.data(), iter->value().data(), key.size(), iter->value().size());
        i++;
      }
        printf("[%s]i: %d\n", fname.c_str(), i);

      // Finish and check for builder errors
      s = builder->Finish();
      if (s.ok()) {
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
      delete builder;

      // Finish and check for file errors
      if (s.ok()) {
        s = file->Sync();
      }
      if (s.ok()) {
        s = file->Close();
      }
      delete file;
      file = nullptr;

      if (s.ok()) {
        // Verify that the table is usable
        Iterator* it = table_cache->NewIterator(ReadOptions(),
                                                meta->number,
                                                meta->file_size);
        s = it->status();
        delete it;
      }

      // Check for input iterator errors
      if (!iter->status().ok()) {
        s = iter->status();
      }


    } else if (sst_type == kPmemSST) {

      TableBuilder* builder = new TableBuilder(options, nullptr);
      meta->smallest.DecodeFrom(iter->key());

      uint64_t file_number;
      FileType type;
      if (ParseFileName(fname.substr(fname.rfind("/")+1, fname.size()), &file_number, &type) &&
            type != kDBLockFile) {
          PmemSkiplist* pmem_skiplist;
          PmemHashmap* pmem_hashmap;
          switch (options.ds_type) {
            case kSkiplist:
              pmem_skiplist = options.pmem_skiplist[file_number%10];
              break;
            case kHashmap:
              pmem_hashmap = options.pmem_hashmap[file_number%10];
              break;
          }
          PmemBuffer* pmem_buffer = options.pmem_buffer[file_number%10];
          // DEBUG:
          // printf("file_number: %d\n", file_number);
          int i =0;
          for (; iter->Valid(); iter->Next()) {
            Slice key = iter->key();
            meta->largest.DecodeFrom(key);
            if (options.use_pmem_buffer) {
              switch (options.ds_type) {
                case kSkiplist:
                  builder->AddToBufferAndSkiplist(pmem_buffer, pmem_skiplist, 
                                              file_number, key, iter->value());
                  break;
                case kHashmap:
                  builder->AddToBufferAndHashmap(pmem_buffer, pmem_hashmap,
                                              file_number, key, iter->value());
                  break;
              }
            } else {
              builder->AddToPmem(pmem_skiplist, file_number, key, iter->value());
            }
            i++;
            // printf("%d]]\n", i);
          }
          // printf("[DEBUG][builder]num_entries: %d\n",i);
          // printf("[DEBUG][builder]file_size: %d\n",builder->FileSize());
          // PROGRESS:
          if(options.use_pmem_buffer) {
            builder->FlushBufferToPmemBuffer(pmem_buffer, file_number);
          }
          // PROGRESS:
          if(options.ds_type == kSkiplist && options.skiplist_cache) {
            Iterator* it = table_cache->NewIteratorFromPmem(ReadOptions(),
                                                  meta->number,
                                                  meta->file_size);
            s = it->status();
            it->RunCleanupFunc();
            // delete it;
          }

      } else {
        printf("[ERROR] Invalid filename '%s' '%d'\n", fname.c_str(), file_number);
        s = Status::InvalidArgument(Slice());
      }
      s = builder->FinishPmem();
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
      delete builder;
      // delete file;
      // file = nullptr;
    }
    
    if (s.ok() && meta->file_size > 0) {
      // Keep it
    } else {
      env->DeleteFile(fname);
    }
  }
  // TODO: Minimize time to insertion
  // std::chrono::steady_clock::time_point end= std::chrono::steady_clock::now();
  // std::cout << "result = " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() <<std::endl;
  return s;
}

}  // namespace leveldb

