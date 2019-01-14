// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <deque>
#include <limits>
#include <set>
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/posix_logger.h"
#include "util/env_posix_test_helper.h"

// JH
#include <iostream>
#include <fstream>
#include "pmem/pmem_file.h"
#include "libpmemobj++/mutex.hpp"
#include "env_posix.cc"

#define FILE0 "/home/hwan/pmem_dir/file0"
#define FILE1 "/home/hwan/pmem_dir/file1"
#define FILE2 "/home/hwan/pmem_dir/file2"
#define FILE3 "/home/hwan/pmem_dir/file3"
#define FILE4 "/home/hwan/pmem_dir/file4"
#define FILE5 "/home/hwan/pmem_dir/file5"
#define FILE6 "/home/hwan/pmem_dir/file6"
#define FILE7 "/home/hwan/pmem_dir/file7"
#define FILE8 "/home/hwan/pmem_dir/file8"
#define FILE9 "/home/hwan/pmem_dir/file9"
#define FILESIZE (1024 * 1024  * 1024 * 1.65) // About 1.65GB
#define FILEID "file"
#define CONTENTS 1600000000 // 1.6GB
#define EACH_CONTENT 4000000
#define CONTENTS_SIZE (CONTENTS/EACH_CONTENT)   // 1.6GB / 4MB
// #define CONTENTS_SIZE 400   // 1.6GB / 4MB

#define OFFSET "/home/hwan/pmem_dir/offset"
#define OFFSETID "offset"
#define OFFSETS_POOL_SIZE (1024 * 1024 * 40) // 40MB 
#define OFFSETS_SIZE 4000 // 40KB 

#define EXFILE "/home/hwan/pmem_dir/exfile"
#define EXFILEID "exfile"
#define EXFILE_SIZE (1024 * 1024 * 40)
#define EXFILE_CONTENTS 10000000 // 10MB
#define EACH_EXFILE_CONTENT 2000000
#define EXFILE_CONTENTS_SIZE (EXFILE_CONTENTS/EACH_EXFILE_CONTENT)  // 10MB / 2MB
// #define EXFILE_CONTENTS_SIZE 5  // 10MB / 2MB

#define EXOFFSET "/home/hwan/pmem_dir/exoffset"
#define EXOFFSETID "exoffset"
#define EXOFFSETS_POOL_SIZE (1024 * 1024 * 8) // 8MB (MIN)
#define EXOFFSETS_SIZE 5 // CURRENT MANIFEST .dbtmp


// #define FILE_SIZE (1024 * 1024 * 12) // @ MB
// #define FILE_SIZE (1024 * 1024 * 1024 * 2) // @ GB

// HAVE_FDATASYNC is defined in the auto-generated port_config.h, which is
// included by port_stdcxx.h.
#if !HAVE_FDATASYNC
#define fdatasync fsync
#endif  // !HAVE_FDATASYNC

namespace leveldb {
 
namespace {

// [pmem] JH
class PmemSequentialFile : public SequentialFile {
 private:
  std::string filename_;
  pobj::pool<rootFile>* pool;
  uint32_t start_offset;
  uint32_t num;

 public:
  PmemSequentialFile(const std::string& fname, pobj::pool<rootFile>* pool, 
                    uint32_t start_offset, uint32_t num)
      : filename_(fname), pool(pool), start_offset(start_offset), num(num) {
  }
  // PmemSequentialFile(const std::string& fname, PmemFile* pmemfile)
  //     : filename_(fname), pmemfile(pmemfile) { 
  // }


  virtual ~PmemSequentialFile() {
    // pool.close();
    // printf("Delete [S]%s\n", filename_.c_str());
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    // std::cout<<"############## Read Seq\n";
    Status s;
    // Get info
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t contents_size, current_index;
    memcpy(&contents_size, ptr->contents_size.get() + (num * sizeof(uint32_t)), 
            sizeof(uint32_t));
    memcpy(&current_index, ptr->current_index.get() + (num * sizeof(uint32_t)), 
            sizeof(uint32_t));
    // printf("[READ DEBUG %d] %d, %d\n",num, contents_size, current_index);
    ssize_t r;

    if (contents_size == current_index) r = 0;
    else {
      // TO DO, run memcpy without buffer-size limitation
      uint32_t avaliable_indexspace = contents_size - current_index;
      if (n > avaliable_indexspace) {
        // printf("1] %d %d\n", start_offset, current_index);
        memcpy(scratch, ptr->contents.get() + start_offset + current_index 
            , avaliable_indexspace);
        r = avaliable_indexspace;
      } else {
        // printf("2] %d %d\n", start_offset, current_index);
        memcpy(scratch, ptr->contents.get() + start_offset + current_index 
            , n);
        r = n;
      }

      
      // r = n;
    }
    *result = Slice(scratch, r);

    // ssize_t r = ptr->file->Read(n, scratch);
    // printf("Read %d \n", r);
    // printf("Read %d] %d '%s' %c\n",r, result->size(), result->data(), scratch[4]);
    
    // Skip r
    Skip(r);
    return s;
  }

  virtual Status Skip(uint64_t n) {
    // std::cout<<"Skip \n";
    Status s;
    // Status s = ptr->file->Skip(n);
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t original_index;
    memcpy(&original_index, ptr->current_index.get() + (num * sizeof(uint32_t)),
            sizeof(uint32_t));
    original_index += n;
    memcpy(ptr->current_index.get() + (num * sizeof(uint32_t)), &original_index,
            sizeof(uint32_t));
    return s;
  }
};

// [pmem] JH
class PmemRandomAccessFile : public RandomAccessFile {
 private:
  std::string filename_;
  pobj::pool<rootFile>* pool;
  uint32_t start_offset;
  uint32_t num;

 public:
  PmemRandomAccessFile(const std::string& fname, pobj::pool<rootFile>* pool, 
                    uint32_t start_offset, uint32_t num)
      : filename_(fname), pool(pool), start_offset(start_offset), num(num) {
  }
  // PmemRandomAccessFile(const std::string& fname)
  //     : filename_(fname) { 
  // }
  // PmemRandomAccessFile(const std::string& fname, PmemFile* pmemfile)
  //     : filename_(fname), pmemfile(pmemfile) { 
  // }

  virtual ~PmemRandomAccessFile() {
    // pool.close();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    // printf("############# %s Read %lld %lld \n", filename_.c_str(), offset, n);
    // ssize_t r = ptr->file->Read(offset, n, scratch);
    // *result = Slice(scratch, (r<0 ? 0 : r));
    // std::cout<<"Read End\n";
    Status s;
    // Get info
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t contents_size;
    memcpy(&contents_size, ptr->contents_size.get() + (num * sizeof(uint32_t)), 
            sizeof(uint32_t));
    // printf("[READ DEBUG %d] %d, %d\n",num, contents_size, current_index);
    ssize_t r;

      // TO DO, run memcpy without buffer-size limitation
      uint32_t avaliable_indexspace = contents_size - offset;
      if (n > avaliable_indexspace) {
        printf("[ERROR] Randomly readable size < n \n");
        // Throw exception

        // memcpy(scratch, ptr->contents.get() + start_offset + offset 
        //     , avaliable_indexspace);
        // r = avaliable_indexspace;
      } else {
        // printf("[RA DEBUG2]\n");
        memcpy(scratch, ptr->contents.get() + start_offset + offset 
            , n);
        r = n;
      // r = n;
    }
    *result = Slice(scratch, r);
    // printf("Read %d %d %d] %d '%s' \n", offset, n, contents_size, result->size(), result->data());
    return s;
  }
};

// [pmem] JH
class PmemWritableFile : public WritableFile {
 private:
  std::string filename_;
  pobj::pool<rootFile>* pool;
  uint32_t start_offset;
  uint32_t num;

 public:
  PmemWritableFile(const std::string& fname, pobj::pool<rootFile>* pool, 
                    uint32_t start_offset, uint32_t num)
      : filename_(fname), pool(pool), start_offset(start_offset), num(num) {
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t zero = 0;
    memcpy(ptr->contents_size.get() + (num * sizeof(uint32_t)), &zero,
            sizeof(uint32_t));
    memcpy(ptr->current_index.get() + (num * sizeof(uint32_t)), &zero,
            sizeof(uint32_t));
      // : filename_(fname), pool(pool), pos_(0), set_ptr_flag(0) { 
  }

  virtual ~PmemWritableFile() { }

  virtual Status Append(const Slice& data) {
    // printf("Append %d] %d %s \n",num, data.size(), data.data());
    
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t contents_size;
    // uint32_t contents_size, current_index;
    memcpy(&contents_size, ptr->contents_size.get() + (num * sizeof(uint32_t)), 
            sizeof(uint32_t));
    // memcpy(&current_index, ptr->current_index.get() + (num * sizeof(uint32_t)), 
    //         sizeof(uint32_t));
    // Append
    memcpy(ptr->contents.get() + start_offset + contents_size, data.data(), 
            data.size());
    contents_size += data.size();
    memcpy(ptr->contents_size.get() + (num * sizeof(uint32_t)), &contents_size,
            sizeof(uint32_t));
    // printf("Append Result] %d\n",contents_size);
    // size_t n = data.size();
    // const char* p = data.data();

    // Fit as much as possible into buffer.
    // size_t copy = std::min(n, kBufSize - pos_); // data size OR remaining buffer size
    // memcpy(buf_ + pos_, p, copy);
    // p += copy;
    // n -= copy;
    // pos_ += copy;

    // if (n == 0) {
    return Status::OK();
    // }

    // Can't fit in buffer, so need to do at least one write.
    // Current buffer -> flush
    // allocate new buffer (pos_ = 0)
    // Status s = FlushBuffered();
    // if (!s.ok()) {
    //   return s;
    // }

    // Small writes go to buffer, large writes are written directly.
    // if (n < kBufSize) {
    //   memcpy(buf_, p, n);
    //   pos_ = n;
    //   return Status::OK();
    // }
    // Without through buffer, write.
    // std::cout<< "After append "<< ptr->file->getContentsSize()<<"\n";
  }

  virtual Status Close() {
    // printf("############ Close %s\n", filename_.c_str());
    // Status result = FlushBuffered();
    // pool.close();
    // return result;
    return Status::OK();
  }

  virtual Status Flush() {
    return FlushBuffered();
  }

  // pobj::persistent_ptr<rootFile> getFilePtr() {
  //   return pool.get_root();
  // };

  Status SyncDirIfManifest() {
    // const char* f = filename_.c_str();
    // const char* sep = strrchr(f, '/');
    // Slice basename;
    // std::string dir;
    // if (sep == nullptr) {
    //   dir = ".";
    //   basename = f;
    // } else {
    //   dir = std::string(f, sep - f);
    //   basename = sep + 1;
    // }
    // Status s;
    // if (basename.starts_with("MANIFEST")) {
    //   int fd = open(dir.c_str(), O_RDONLY);
    //   if (fd < 0) {
    //     s = PosixError(dir, errno);
    //   } else {
    //     if (fsync(fd) < 0) {
    //       s = PosixError(dir, errno);
    //     }
    //     close(fd);
    //   }
    // }
    // return s;
    return Status::OK();
  }

  virtual Status Sync() {
    // printf("############# Sync\n");
    // Ensure new files referred to by the manifest are in the filesystem.
    // Status s = SyncDirIfManifest();
    // if (!s.ok()) {
    //   return s;
    // }
    // s = FlushBuffered();
    // if (s.ok()) {
    //   if (fdatasync(fd_) != 0) {
    //     s = PosixError(filename_, errno);
    //   }
    // }
    // return s;
    return Status::OK();
  }

 private:
  Status FlushBuffered() {
    // Status s = WriteRaw(buf_, pos_);
    // pos_ = 0;
    // return s;
    return Status::OK();
  }

  Status WriteRaw(const char* p, size_t n) {
    // while (n > 0) {
    //   // Slice data = Slice(p, n);
    //   // printf("WriteRaw %d in %s\n", n, filename_.c_str());
    //   ssize_t r;
    //   // ssize_t r = ptr->file->Append(p, n);
    //   // printf("WriteRaw %d %s %d\n", n, p, r);
    //   // ssize_t r = ptr->file->Append(data);      
    //   // printf("WriteRaw '%c' '%c' '%c' '%c' '%c' '%c' '%c'\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    //   // char buf;
    //   // memcpy(&buf, p+6, sizeof(char));
    //   // printf("WriteRaw %d '%d'\n", n, p[6]);
    //   // printf("WriteRaw %d %s %d\n", data.size(), data.data(), r);
    //   // ssize_t r = write(fd_, p, n);
    //   if (r < 0) {
    //     if (errno == EINTR) {
    //       continue;  // Retry
    //     }
    //     return PosixError(filename_, errno);
    //   }
    //   p += r;
    //   n -= r;
    // }
    return Status::OK();
  }
};


// class PosixFileLock : public FileLock {
//  public:
//   int fd_;
//   std::string name_;
// };

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
// class PosixLockTable {
//  private:
//   port::Mutex mu_;
//   std::set<std::string> locked_files_ GUARDED_BY(mu_);
//  public:
//   bool Insert(const std::string& fname) LOCKS_EXCLUDED(mu_) {
//     MutexLock l(&mu_);
//     return locked_files_.insert(fname).second;
//   }
//   void Remove(const std::string& fname) LOCKS_EXCLUDED(mu_) {
//     MutexLock l(&mu_);
//     locked_files_.erase(fname);
//   }
// };

class PmemEnv : public Env {
 public:
  PmemEnv();
  virtual ~PmemEnv() {
    char msg[] = "Destroying Env::Default()\n";
    fwrite(msg, 1, sizeof(msg), stderr);

    // JH
    // pobj::delete_persistent<rootDirectory>(Dir_ptr);
    // Dir_pool.close();

    abort();
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    std::cout<< "NewSequentialFile "<<fname<<" \n";
    Status s;
    // 1) Get info
    int32_t num = GetFileNumber(fname);
    pobj::pool<rootFile>* pool = GetPool(num);
    uint32_t offset = GetOffset(fname, num);
      // printf("num %d offset %d\n", num, offset);
    // Special file
    if (num == -1) {
      num = GetExtraFileNumber(fname);
      // Get renamed number
      if (num == 0) num = offset / EACH_EXFILE_CONTENT;
      // SetExtraOffset(num, offset);
    } 
    // Normal file
    else {
      // Set offset
      // SetOffset(num, offset);
      if (offset == -1) {
        printf("[ERROR] Invalid offset ( The number of files >= 4000(Limit)) \n");
      }
    }

    // *result = new PmemSequentialFile(fname, pool, offset, num);

    // Add empty file
    // int fd = open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT, 0644);
    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      *result = nullptr;
      s = PosixError(fname, errno);
    } else {
      *result = new PmemSequentialFile(fname, pool, offset, num);
    }
    return s;

  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) {
    std::cout<< "NewRandomAccessFile "<<fname<<" \n";
    Status s;
    // 1) Get info
    int32_t num = GetFileNumber(fname);
    pobj::pool<rootFile>* pool = GetPool(num);
    uint32_t offset = GetOffset(fname, num);
      // printf("num %d offset %d\n", num, offset);
    // Special file
    if (num == -1) {
      num = GetExtraFileNumber(fname);
      // Get renamed number
      if (num == 0) num = offset / EACH_EXFILE_CONTENT;
      // SetExtraOffset(num, offset);
    } 
    // Normal file
    else {
      // Set offset
      // SetOffset(num, offset);
      if (offset == -1) {
        printf("[ERROR] Invalid offset ( The number of files >= 4000(Limit)) \n");
      }
    }
    // *result = new PmemRandomAccessFile(fname, &ptr->files[num]);
    // Add empty file
    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      *result = nullptr;
      s = PosixError(fname, errno);
    } else {
      *result = new PmemRandomAccessFile(fname, pool, offset, num);
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    std::cout<< "NewWritableFile "<<fname<<" \n";
    Status s;
    // 1) Get info
    int32_t num = GetFileNumber(fname);
    pobj::pool<rootFile>* pool = GetPool(num);
    // printf("NewWritable Debug1\n");
    uint32_t offset = GetOffset(fname, num);
    // printf("num: %d offset: %d\n",num, offset);
    // Special file
    if (num == -1) {
      num = GetExtraFileNumber(fname);
      SetExtraOffset(num, offset);
    } 
    // Normal file
    else {
      // Set offset
      SetOffset(num, offset);
      if (offset == -1) {
        printf("[ERROR] Invalid offset ( The number of files >= 4000(Limit)) \n");
      }
    }
    // printf("NewWritable Debug3\n");

    // *result = new PmemWritableFile(fname, pool, offset, num);

    // Add empty file
    int fd = open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
      *result = nullptr;
      s = PosixError(fname, errno);
    } else {
      *result = new PmemWritableFile(fname, pool, offset, num);
    }
    return s;
  }

  virtual Status NewAppendableFile(const std::string& fname,
                                   WritableFile** result) {
    std::cout<< "NewAppendableFile \n";
    Status s;
    int32_t num = GetFileNumber(fname);

    // *result = new PmemWritableFile(fname, &ptr->files[num]);

    return s;
  }

  virtual bool FileExists(const std::string& fname) {
    // printf("Exists %s\n", fname.c_str());
    return access(fname.c_str(), F_OK) == 0;
  }
  // JH
  virtual int32_t GetFileNumber(const std::string& fname) {
    std::string slash = "/";
    std::string fileNumber = fname.substr( fname.rfind(slash)+1, fname.size() );

    // Filter tmp file and MANIFEST file
    if (fileNumber.find("dbtmp") != std::string::npos
        || fileNumber.find("MANIFEST") != std::string::npos
        || fileNumber.find("CURRENT") != std::string::npos) {
      return -1;
    }
    return std::atoi(fileNumber.c_str());
  }
  // For MANIFEST, dbtmp, CURRENT file
  virtual int32_t GetExtraFileNumber(const std::string& fname) {
    std::string slash = "/"; std::string dash = "-";
    std::string fileNumber = fname.substr( fname.rfind(slash)+1, fname.size() );

    int32_t res;
    // Filter tmp file and MANIFEST file
    if (fileNumber.find("dbtmp") != std::string::npos) {
      switch (std::atoi(fileNumber.c_str())) {
        case 1:
          res = 1;
          break;
        case 2:
          res = 2;
          break;
        default:
          printf("[ERROR] Invalid dbtmp number... \n");
          // throw exception
          break;
      }
    } else if (fileNumber.find("MANIFEST") != std::string::npos) {
      std::string tmp = fileNumber.substr(fileNumber.find(dash)+1, fileNumber.size());
      switch (std::atoi(tmp.c_str())) {
        case 1:
          res = 3;
          break;
        case 2:
          res = 4;
          break;
        default:
          printf("[ERROR] Invalid MANIFEST number... \n");
          // throw exception
          break;
      }
    } else if (fileNumber.find("CURRENT") != std::string::npos) {
      res = 0;
    }
    return res;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
      return PosixError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }
  // Customized by JH
  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (fname.find("MANIFEST") != std::string::npos
        || fname.find(".log") != std::string::npos
        || fname.find("CURRENT") != std::string::npos) {

      // printf("[DELETE] %s\n", fname.c_str());
      int32_t num = GetFileNumber(fname);
      pobj::pool<rootFile> *pool = GetPool(num);
      
      // 1) Get info
      uint32_t offset = GetOffset(fname, num);
  // printf("111 %d %d\n",num,  offset);
      // 2) Reset file
      // Extra file
      if (num == -1) {
        num = GetExtraFileNumber(fname);
        ResetFile(pool, num);
        // Reset .dbtmp fname(num)&offset
        if (fname.find("CURRENT") != std::string::npos) {
          // printf("%d %d\n", num, offset);
          SetExtraOffset(1, offset);
          SetExtraOffset(2, offset);
          // for (int i=0; i<5; i++) {
          //   printf("[i %d]\n", i);
          //   int32_t tmp;
          //   memcpy(&tmp, exOffsets->fname.get() + (i * sizeof(uint32_t)), 
          //           sizeof(uint32_t));
          //   printf("tmp: %d\n",tmp);
          // }
        }
      } 
      // Normal file
      else {
        ResetFile(pool, num);
      // TO DO, dynamic alloc (out of boundary)
      // int index = (num / 10);
        // Reset offset (for dynamic alloc) 
        uint32_t offset_reset_value = 0;
        SetOffset(num, offset_reset_value);
      }
      if (unlink(fname.c_str()) != 0) {
        result = PosixError(fname, errno);
      }
    }
    else if (fname.find("file") != std::string::npos
              || fname.find("offset") != std::string::npos) {
      printf("[INFO] Do not delete file, offset\n");
    }
    // Original path
    else {
      if (unlink(fname.c_str()) != 0) {
        result = PosixError(fname, errno);
      }
    }
    return result;
  }

  virtual Status CreateDir(const std::string& name) {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = PosixError(name, errno);
    }
    return result;
  }

  // Customized by JH
  virtual Status DeleteDir(const std::string& name) {
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = PosixError(name, errno);
    }
    return result;
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = PosixError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  // For special-file
  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;

    // Just change offset's fname to 0
    if (src.find("dbtmp") != std::string::npos
        && target.find("CURRENT") != std::string::npos) {
      int32_t src_num = GetExtraFileNumber(src);
      uint32_t src_offset = GetExtraOffset(src);
      // for (int i=0; i<5; i++) {
      //   printf("[i %d]\n", i);
      //   int32_t tmp;
      //   memcpy(&tmp, exOffsets->fname.get() + (i * sizeof(uint32_t)), 
      //           sizeof(uint32_t));
      //   printf("tmp: %d\n",tmp);
      // }
      int32_t zero = 0; // CURRENT index
      memcpy(exOffsets->fname.get() + (src_num * sizeof(uint32_t)), &zero, 
              sizeof(int32_t));    
      // for (int i=0; i<5; i++) {
      //   printf("[i %d]\n", i);
      //   int32_t tmp;
      //   memcpy(&tmp, exOffsets->fname.get() + (i * sizeof(uint32_t)), 
      //           sizeof(uint32_t));
      //   printf("tmp: %d\n",tmp);
      // }
      // printf("[RENAME] %s->%s\n", src.c_str(), target.c_str());
    } 
    // Original path (LOG -> LOG.old)
    // else {
      if (rename(src.c_str(), target.c_str()) != 0) {
        result = PosixError(src, errno);
      }
    // }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = nullptr;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = PosixError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock(fd, true) == -1) {
      result = PosixError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->fd_, false) == -1) {
      result = PosixError("unlock", errno);
    }
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
    FILE* f = fopen(fname.c_str(), "w");
    if (f == nullptr) {
      *result = nullptr;
      return PosixError(fname, errno);
    } else {
      *result = new PosixLogger(f, &PmemEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }
  // JH
  virtual pobj::pool<rootFile>* GetPool(const uint32_t num) {
    pobj::pool<rootFile> *pool;
    switch (num % 10) {
      case 0: pool = &filePool0; break;
      case 1: pool = &filePool1; break;
      case 2: pool = &filePool2; break;
      case 3: pool = &filePool3; break;
      case 4: pool = &filePool4; break;
      case 5: pool = &filePool5; break;
      case 6: pool = &filePool6; break;
      case 7: pool = &filePool7; break;
      case 8: pool = &filePool8; break;
      case 9: pool = &filePool9; break;
      case -1: pool = &exfilePool; break;
      default: 
        printf("[ERROR] Invalid File name type...\n");
        // Throw exception
        break;
    }
    return pool;
  }
  virtual uint32_t GetOffset(const std::string& fname, const int32_t num) {
    uint32_t offset;
    // Check num range
    if (num < 0) {
      // dbtmp, MANIFEST
      offset = GetExtraOffset(fname);
    } else if (num <= OFFSETS_SIZE) {
      // Pre-allocated area
      offset = (num / 10) * EACH_CONTENT; // based on 10-pools
    } 
    // TO DO, extend all range with dynamic-allocation
    else {
      offset = -1;
      // Temp, throw exception
      printf("[ERROR] need more offset..\n");
    }
    return offset;
  }
  virtual uint32_t GetExtraOffset(const std::string& fname) {
    int32_t num = GetExtraFileNumber(fname);
    if (num == 0) {
      // Seek CURRENT file (for renaming)
      for (int i=0; i<5; i++) {
        // printf("[i %d]\n", i);
        int32_t tmp;
        memcpy(&tmp, exOffsets->fname.get() + (i * sizeof(uint32_t)), 
                sizeof(uint32_t));
        // printf("tmp: %d\n",tmp);
        if (tmp == 0) {
          num = i;
          // printf("num %d\n",num);
        }
      }
    }
    return num * EACH_EXFILE_CONTENT;
  }
  virtual void SetOffset(const uint32_t num, const uint32_t offset) {
    memcpy(offsets->fname.get() + (num * sizeof(uint32_t)), 
            &num, sizeof(uint32_t));
    memcpy(offsets->start_offset.get() + (num * sizeof(uint32_t)), 
            &offset, sizeof(uint32_t));
  }
  virtual void SetExtraOffset(const uint32_t num, const uint32_t offset) {
    memcpy(exOffsets->fname.get() + (num * sizeof(uint32_t)), 
            &num, sizeof(uint32_t));
    memcpy(exOffsets->start_offset.get() + (num * sizeof(uint32_t)), 
            &offset, sizeof(uint32_t));
  }
  virtual void ResetFile(pobj::pool<rootFile>* pool, const uint32_t num) {
    pobj::persistent_ptr<rootFile> ptr = pool->get_root();
    uint32_t zero = 0;
    memcpy(ptr->contents_size.get() + (num * sizeof(uint32_t)), &zero, sizeof(uint32_t));
    memcpy(ptr->current_index.get() + (num * sizeof(uint32_t)), &zero, sizeof(uint32_t));
  };

 private:
  void PthreadCall(const char* label, int result) {
    if (result != 0) {
      fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
      abort();
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PmemEnv*>(arg)->BGThread();
    return nullptr;
  }

  pthread_mutex_t mu_;
  pthread_cond_t bgsignal_;
  pthread_t bgthread_;
  bool started_bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  PosixLockTable locks_;
  Limiter mmap_limit_;
  Limiter fd_limit_;

  // JH
  // 10-filePools & ptr (400Files * 10 = 4000Files)
  pobj::pool<rootFile> filePool0;
  pobj::pool<rootFile> filePool1;
  pobj::pool<rootFile> filePool2;
  pobj::pool<rootFile> filePool3;
  pobj::pool<rootFile> filePool4;
  pobj::pool<rootFile> filePool5;
  pobj::pool<rootFile> filePool6;
  pobj::pool<rootFile> filePool7;
  pobj::pool<rootFile> filePool8;
  pobj::pool<rootFile> filePool9;
  // pobj::persistent_ptr<rootFile> file0;
  // pobj::persistent_ptr<rootFile> file1;
  // pobj::persistent_ptr<rootFile> file2;
  // pobj::persistent_ptr<rootFile> file3;
  // pobj::persistent_ptr<rootFile> file4;
  // pobj::persistent_ptr<rootFile> file5;
  // pobj::persistent_ptr<rootFile> file6;
  // pobj::persistent_ptr<rootFile> file7;
  // pobj::persistent_ptr<rootFile> file8;
  // pobj::persistent_ptr<rootFile> file9;

  // 1-extra-filePool & ptr for MANIFEST, tmp file
  pobj::pool<rootFile> exfilePool;
  // pobj::persistent_ptr<rootFile> exfile;

  // 1-offsetPool (contains 4000Files )
  pobj::pool<rootOffset> offsetPool;
  pobj::persistent_ptr<rootOffset> offsets;

  // 1-extra-offsetPool (contains 2~5 Files )
  pobj::pool<rootOffset> exOffsetPool;
  pobj::persistent_ptr<rootOffset> exOffsets;

};

PmemEnv::PmemEnv()
    : started_bgthread_(false),
      mmap_limit_(MaxMmaps()),
      fd_limit_(MaxOpenFiles()) {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
  PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, nullptr));
  printf("PmemEnv start\n");
  // JH
  // tmp for initializing
  pobj::persistent_ptr<rootFile> file0;
  pobj::persistent_ptr<rootFile> file1;
  pobj::persistent_ptr<rootFile> file2;
  pobj::persistent_ptr<rootFile> file3;
  pobj::persistent_ptr<rootFile> file4;
  pobj::persistent_ptr<rootFile> file5;
  pobj::persistent_ptr<rootFile> file6;
  pobj::persistent_ptr<rootFile> file7;
  pobj::persistent_ptr<rootFile> file8;
  pobj::persistent_ptr<rootFile> file9;

  // Initialize all pools (10-files, 1-offset)
  // 1) FILE BUFFER
  if (!FileExists(FILE0)) {
    // std::cout<< "New PmemFile "<<FILE0<<" \n";
    filePool0 = pobj::pool<rootFile>::create (FILE0, FILEID,
        // 2GB cause error
        // About 1.65GB
        FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool1 = pobj::pool<rootFile>::create (FILE1, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool2 = pobj::pool<rootFile>::create (FILE2, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool3 = pobj::pool<rootFile>::create (FILE3, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool4 = pobj::pool<rootFile>::create (FILE4, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool5 = pobj::pool<rootFile>::create (FILE5, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool6 = pobj::pool<rootFile>::create (FILE6, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool7 = pobj::pool<rootFile>::create (FILE7, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool8 = pobj::pool<rootFile>::create (FILE8, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    filePool9 = pobj::pool<rootFile>::create (FILE9, FILEID, FILESIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    // printf("After creating pool\n");


    file0 = filePool0.get_root(); file1 = filePool1.get_root();
    file2 = filePool2.get_root(); file3 = filePool3.get_root();
    file4 = filePool4.get_root(); file5 = filePool5.get_root();
    file6 = filePool6.get_root(); file7 = filePool7.get_root();
    file8 = filePool8.get_root(); file9 = filePool9.get_root();

    pobj::transaction::exec_tx(filePool0, [&] {
      file0->contents = pobj::make_persistent<char[]>(CONTENTS);
      file0->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file0->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool1, [&] {
      file1->contents = pobj::make_persistent<char[]>(CONTENTS);
      file1->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file1->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool2, [&] {
      file2->contents = pobj::make_persistent<char[]>(CONTENTS);
      file2->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file2->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool3, [&] {
      file3->contents = pobj::make_persistent<char[]>(CONTENTS);
      file3->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file3->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool4, [&] {
      file4->contents = pobj::make_persistent<char[]>(CONTENTS);
      file4->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file4->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool5, [&] {
      file5->contents = pobj::make_persistent<char[]>(CONTENTS);
      file5->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file5->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool6, [&] {
      file6->contents = pobj::make_persistent<char[]>(CONTENTS);
      file6->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file6->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool7, [&] {
      file7->contents = pobj::make_persistent<char[]>(CONTENTS);
      file7->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file7->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool8, [&] {
      file8->contents = pobj::make_persistent<char[]>(CONTENTS);
      file8->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file8->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    pobj::transaction::exec_tx(filePool9, [&] {
      file9->contents = pobj::make_persistent<char[]>(CONTENTS);
      file9->contents_size = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);  
      file9->current_index = pobj::make_persistent<uint32_t[]>(CONTENTS_SIZE);
    });
    // printf("Finish all initialization \n");
  } else {
    // std::cout<< "Reuse PmemFile "<<FILE0<<" \n";
    filePool0 = pobj::pool<rootFile>::open (FILE0, FILEID);
    filePool1 = pobj::pool<rootFile>::open (FILE1, FILEID);
    filePool2 = pobj::pool<rootFile>::open (FILE2, FILEID);
    filePool3 = pobj::pool<rootFile>::open (FILE3, FILEID);
    filePool4 = pobj::pool<rootFile>::open (FILE4, FILEID);
    filePool5 = pobj::pool<rootFile>::open (FILE5, FILEID);
    filePool6 = pobj::pool<rootFile>::open (FILE6, FILEID);
    filePool7 = pobj::pool<rootFile>::open (FILE7, FILEID);
    filePool8 = pobj::pool<rootFile>::open (FILE8, FILEID);
    filePool9 = pobj::pool<rootFile>::open (FILE9, FILEID);
    // file0 = filePool0.get_root();
    // file1 = filePool1.get_root();
    // file2 = filePool2.get_root();
    // file3 = filePool3.get_root();
    // file4 = filePool4.get_root();
    // file5 = filePool5.get_root();
    // file6 = filePool6.get_root();
    // file7 = filePool7.get_root();
    // file8 = filePool8.get_root();
    // file9 = filePool9.get_root();
  } 
  // OFFSET BUFFER
  if (!FileExists(OFFSET)) {
    // std::cout<< "New OffsetFile "<<OFFSET<<" \n";
    offsetPool = pobj::pool<rootOffset>::create (OFFSET, OFFSETID,
        OFFSETS_POOL_SIZE , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    // printf("After creating pool\n");

    offsets = offsetPool.get_root();
    // 4000Files
    pobj::transaction::exec_tx(offsetPool, [&] {
      offsets->fname       = pobj::make_persistent<uint32_t[]>(OFFSETS_SIZE);  
      offsets->start_offset = pobj::make_persistent<uint32_t[]>(OFFSETS_SIZE);
    });

  } else {
    // std::cout<< "Reuse OffsetFile "<<FILE0<<" \n";
    offsetPool = pobj::pool<rootOffset>::open (OFFSET, OFFSETID);
    offsets = offsetPool.get_root();
  }
  // tmp for initializing
  pobj::persistent_ptr<rootFile> exfile;
  // EXTRA CONTENTS BUFFER
  if (!FileExists(EXFILE)) {
    exfilePool = pobj::pool<rootFile>::create (EXFILE, EXFILEID, 
        EXFILE_SIZE, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    exfile = exfilePool.get_root();
    pobj::transaction::exec_tx(exfilePool, [&] {
      exfile->contents = pobj::make_persistent<char[]>(EXFILE_CONTENTS);
      exfile->contents_size = pobj::make_persistent<uint32_t[]>(EXFILE_CONTENTS_SIZE);  
      exfile->current_index = pobj::make_persistent<uint32_t[]>(EXFILE_CONTENTS_SIZE);
    });
  } else {
    // printf("exfile pool\n");
    exfilePool = pobj::pool<rootFile>::open (EXFILE, EXFILEID);
    // exfile = exfilePool.get_root();
  }
  // EXTRA OFFSET BUFFER
  if (!FileExists(EXOFFSET)) {
    exOffsetPool = pobj::pool<rootOffset>::create (EXOFFSET, EXOFFSETID, 
        EXOFFSETS_POOL_SIZE, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    exOffsets = exOffsetPool.get_root();
    pobj::transaction::exec_tx(exOffsetPool, [&] {
      exOffsets->fname       = pobj::make_persistent<uint32_t[]>(EXOFFSETS_SIZE);  
      exOffsets->start_offset = pobj::make_persistent<uint32_t[]>(EXOFFSETS_SIZE);
    });
  } else {
    // printf("exoffset pool\n");
    exOffsetPool = pobj::pool<rootOffset>::open (EXOFFSET, EXOFFSETID);
    exOffsets = exOffsetPool.get_root();
  }
    // printf("set extraoffset\n");
  // Initialize for preventing from renaming confusion
  for (int i=0; i<5; i++) {
    SetExtraOffset(i, i*EACH_EXFILE_CONTENT);
  }
    printf("End all setting\n");
}

void PmemEnv::Schedule(void (*function)(void*), void* arg) {
  PthreadCall("lock", pthread_mutex_lock(&mu_));

  // Start background thread if necessary
  if (!started_bgthread_) {
    started_bgthread_ = true;
    PthreadCall(
        "create thread",
        pthread_create(&bgthread_, nullptr,  &PmemEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  if (queue_.empty()) {
    PthreadCall("signal", pthread_cond_signal(&bgsignal_));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PmemEnv::BGThread() {
  while (true) {
    // Wait until there is an item that is ready to run
    PthreadCall("lock", pthread_mutex_lock(&mu_));
    while (queue_.empty()) {
      PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
    }

    void (*function)(void*) = queue_.front().function;
    void* arg = queue_.front().arg;
    queue_.pop_front();

    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    (*function)(arg);
  }
}

// namespace {
// struct StartThreadState {
//   void (*user_function)(void*);
//   void* arg;
// };
// }
// static void* StartThreadWrapper(void* arg) {
//   StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
//   state->user_function(state->arg);
//   delete state;
//   return nullptr;
// }

void PmemEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, nullptr,  &StartThreadWrapper, state));
}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PmemEnv; }
// static void InitDefaultEnv() { default_env = new PosixEnv; }

void EnvPosixTestHelper::SetReadOnlyFDLimit(int limit) {
  assert(default_env == nullptr);
  open_read_only_file_limit = limit;
}

void EnvPosixTestHelper::SetReadOnlyMMapLimit(int limit) {
  assert(default_env == nullptr);
  mmap_limit = limit;
}

Env* Env::Default() {
  pthread_once(&once, InitDefaultEnv);
  return default_env;
}

}  // namespace leveldb
