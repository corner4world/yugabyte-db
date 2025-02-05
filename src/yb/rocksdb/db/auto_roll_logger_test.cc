//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <algorithm>

#include "yb/rocksdb/db/auto_roll_logger.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/util/sync_point.h"
#include "yb/rocksdb/util/testharness.h"
#include "yb/rocksdb/db.h"

namespace rocksdb {

class AutoRollLoggerTest : public testing::Test {
 public:
  static void InitTestDb() {
#ifdef OS_WIN
    // Replace all slashes in the path so windows CompSpec does not
    // become confused
    std::string testDir(kTestDir);
    std::replace_if(testDir.begin(), testDir.end(),
                    [](char ch) { return ch == '/'; }, '\\');
    std::string deleteCmd = "if exist " + testDir + " rd /s /q " + testDir;
#else
    std::string deleteCmd = "rm -rf " + kTestDir;
#endif
    ASSERT_EQ(system(deleteCmd.c_str()), 0);
    Env::Default()->CreateDir(kTestDir);
  }

  void RollLogFileBySizeTest(AutoRollLogger* logger,
                             size_t log_max_size,
                             const string& log_message);
  uint64_t RollLogFileByTimeTest(AutoRollLogger* logger,
                                 size_t time,
                                 const string& log_message);

  static const string kSampleMessage;
  static const string kTestDir;
  static const string kLogFile;
  static Env* env;
};

const string AutoRollLoggerTest::kSampleMessage(
    "this is the message to be written to the log file!!");
const string AutoRollLoggerTest::kTestDir(test::TmpDir() + "/db_log_test");
const string AutoRollLoggerTest::kLogFile(test::TmpDir() + "/db_log_test/LOG");
Env* AutoRollLoggerTest::env = Env::Default();

// In this test we only want to Log some simple log message with
// no format. LogMessage() provides such a simple interface and
// avoids the [format-security] warning which occurs when you
// call Log(logger, log_message) directly.
namespace {
void LogMessage(Logger* logger, const char* message) {
  RLOG(logger, "%s", message);
}

void LogMessage(const InfoLogLevel log_level, Logger* logger,
                const char* message) {
  RLOG(log_level, logger, "%s", message);
}
}  // namespace

namespace {

uint64_t GetFileCreateTime(const std::string& fname) {
  struct stat s;
  if (stat(fname.c_str(), &s) == 0) {
    return static_cast<uint64_t>(s.st_ctime);
  } else {
    const string error_str(strerror(errno));
    std::cerr << "Failed to get creation time of '" << fname << "': " << error_str << std::endl;
    return 0;
  }
}

// Get the inode of the given file as a signed 64-bit integer, or -1 in case of failure.
int64_t GetFileInode(const std::string& fname) {
  struct stat s;
  if (stat(fname.c_str(), &s) == 0) {
    return int64_t(s.st_ino);
  } else {
    const string error_str(strerror(errno));
    std::cerr << "Failed to get inode of '" << fname << "': " << error_str << std::endl;
    return -1;
  }
}

}  // namespace

void AutoRollLoggerTest::RollLogFileBySizeTest(AutoRollLogger* logger,
                                               size_t log_max_size,
                                               const string& log_message) {
  logger->SetInfoLogLevel(InfoLogLevel::INFO_LEVEL);
  // measure the size of each message, which is supposed
  // to be equal or greater than log_message.size()
  LogMessage(logger, log_message.c_str());
  size_t message_size = logger->GetLogFileSize();
  size_t current_log_size = message_size;

  // Test the cases when the log file will not be rolled.
  while (current_log_size + message_size < log_max_size) {
    LogMessage(logger, log_message.c_str());
    current_log_size += message_size;
    ASSERT_EQ(current_log_size, logger->GetLogFileSize());
  }

  // Now the log file will be rolled
  LogMessage(logger, log_message.c_str());
  // Since rotation is checked before actual logging, we need to
  // trigger the rotation by logging another message.
  LogMessage(logger, log_message.c_str());

  ASSERT_TRUE(message_size == logger->GetLogFileSize());
}

uint64_t AutoRollLoggerTest::RollLogFileByTimeTest(
    AutoRollLogger* logger, size_t time, const string& log_message) {
  uint64_t total_log_size = 0;
  EXPECT_OK(env->GetFileSize(kLogFile, &total_log_size));
  const int64_t expected_inode = GetFileInode(kLogFile);
  logger->SetCallNowMicrosEveryNRecords(0);
  const uint64_t initial_create_time = GetFileCreateTime(kLogFile);

  // -- Write to the log for several times, which is supposed
  // to be finished before time.
  for (int i = 0; i < 10; ++i) {
     LogMessage(logger, log_message.c_str());
     EXPECT_OK(logger->GetStatus());
     // Make sure we always write to the same log file (by checking the inode).
     EXPECT_EQ(expected_inode, GetFileInode(kLogFile));

     // Also make sure the log size is increasing.
     EXPECT_GT(logger->GetLogFileSize(), total_log_size);
     total_log_size = logger->GetLogFileSize();
  }

  // -- Make the log file expire
#ifdef OS_WIN
  Sleep(static_cast<unsigned int>(time) * 1000);
#else
  sleep(static_cast<unsigned int>(time));
#endif
  LogMessage(logger, log_message.c_str());

  // At this time, the new log file should be created.

  const uint64_t actual_create_time = GetFileCreateTime(kLogFile);
  EXPECT_GT(actual_create_time, initial_create_time);
  EXPECT_LT(logger->GetLogFileSize(), total_log_size);

  return actual_create_time;
}

TEST_F(AutoRollLoggerTest, RollLogFileBySize) {
    InitTestDb();
    size_t log_max_size = 1024 * 5;

    AutoRollLogger logger(Env::Default(), kTestDir, "", log_max_size, 0);

    RollLogFileBySizeTest(&logger, log_max_size,
                          kSampleMessage + ":RollLogFileBySize");
}

TEST_F(AutoRollLoggerTest, RollLogFileByTime) {
    size_t time = 2;
    size_t log_size = 1024 * 5;

    InitTestDb();
    // -- Test the existence of file during the server restart.
    ASSERT_TRUE(env->FileExists(kLogFile).IsNotFound());
    AutoRollLogger logger(Env::Default(), kTestDir, "", log_size, time);
    ASSERT_OK(env->FileExists(kLogFile));

    RollLogFileByTimeTest(&logger, time, kSampleMessage + ":RollLogFileByTime");
}

TEST_F(AutoRollLoggerTest, OpenLogFilesMultipleTimesWithOptionLog_max_size) {
  // If only 'log_max_size' options is specified, then every time
  // when rocksdb is restarted, a new empty log file will be created.
  InitTestDb();
  // WORKAROUND:
  // avoid complier's complaint of "comparison between signed
  // and unsigned integer expressions" because literal 0 is
  // treated as "singed".
  size_t kZero = 0;
  size_t log_size = 1024;

  AutoRollLogger* logger = new AutoRollLogger(
    Env::Default(), kTestDir, "", log_size, 0);

  LogMessage(logger, kSampleMessage.c_str());
  ASSERT_GT(logger->GetLogFileSize(), kZero);
  delete logger;

  // reopens the log file and an empty log file will be created.
  logger = new AutoRollLogger(
    Env::Default(), kTestDir, "", log_size, 0);
  ASSERT_EQ(logger->GetLogFileSize(), kZero);
  delete logger;
}

TEST_F(AutoRollLoggerTest, CompositeRollByTimeAndSizeLogger) {
  size_t time = 2, log_max_size = 1024 * 5;

  InitTestDb();

  AutoRollLogger logger(Env::Default(), kTestDir, "", log_max_size, time);

  // Test the ability to roll by size
  RollLogFileBySizeTest(
      &logger, log_max_size,
      kSampleMessage + ":CompositeRollByTimeAndSizeLogger");

  // Test the ability to roll by Time
  RollLogFileByTimeTest( &logger, time,
      kSampleMessage + ":CompositeRollByTimeAndSizeLogger");
}

#ifndef OS_WIN
// TODO: does not build for Windows because of PosixLogger use below. Need to
// port
TEST_F(AutoRollLoggerTest, CreateLoggerFromOptions) {
  DBOptions options;
  shared_ptr<Logger> logger;

  // Normal logger
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, options, &logger));
  ASSERT_TRUE(dynamic_cast<PosixLogger*>(logger.get()));

  // Only roll by size
  InitTestDb();
  options.max_log_file_size = 1024;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, options, &logger));
  AutoRollLogger* auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  ASSERT_TRUE(auto_roll_logger);
  RollLogFileBySizeTest(
      auto_roll_logger, options.max_log_file_size,
      kSampleMessage + ":CreateLoggerFromOptions - size");

  // Only roll by Time
  InitTestDb();
  options.max_log_file_size = 0;
  options.log_file_time_to_roll = 2;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, options, &logger));
  auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  RollLogFileByTimeTest(
      auto_roll_logger, options.log_file_time_to_roll,
      kSampleMessage + ":CreateLoggerFromOptions - time");

  // roll by both Time and size
  InitTestDb();
  options.max_log_file_size = 1024 * 5;
  options.log_file_time_to_roll = 2;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, options, &logger));
  auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  RollLogFileBySizeTest(
      auto_roll_logger, options.max_log_file_size,
      kSampleMessage + ":CreateLoggerFromOptions - both");
  RollLogFileByTimeTest(
      auto_roll_logger, options.log_file_time_to_roll,
      kSampleMessage + ":CreateLoggerFromOptions - both");
}

TEST_F(AutoRollLoggerTest, LogFlushWhileRolling) {
  DBOptions options;
  shared_ptr<Logger> logger;

  InitTestDb();
  options.max_log_file_size = 1024 * 5;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, options, &logger));
  AutoRollLogger* auto_roll_logger =
      dynamic_cast<AutoRollLogger*>(logger.get());
  ASSERT_TRUE(auto_roll_logger);
  std::thread flush_thread;

  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      // Need to pin the old logger before beginning the roll, as rolling grabs
      // the mutex, which would prevent us from accessing the old logger.
      {"AutoRollLogger::Flush:PinnedLogger",
       "AutoRollLoggerTest::LogFlushWhileRolling:PreRollAndPostThreadInit"},
      // Need to finish the flush thread init before this callback because the
      // callback accesses flush_thread.get_id() in order to apply certain sync
      // points only to the flush thread.
      {"AutoRollLoggerTest::LogFlushWhileRolling:PreRollAndPostThreadInit",
       "AutoRollLoggerTest::LogFlushWhileRolling:FlushCallbackBegin"},
      // Need to reset logger at this point in Flush() to exercise a race
      // condition case, which is executing the flush with the pinned (old)
      // logger after the roll has cut over to a new logger.
      {"AutoRollLoggerTest::LogFlushWhileRolling:FlushCallback1",
       "AutoRollLogger::ResetLogger:BeforeNewLogger"},
      {"AutoRollLogger::ResetLogger:AfterNewLogger",
       "AutoRollLoggerTest::LogFlushWhileRolling:FlushCallback2"},
  });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "PosixLogger::Flush:BeginCallback", [&](void* arg) {
        TEST_SYNC_POINT(
            "AutoRollLoggerTest::LogFlushWhileRolling:FlushCallbackBegin");
        if (std::this_thread::get_id() == flush_thread.get_id()) {
          TEST_SYNC_POINT(
              "AutoRollLoggerTest::LogFlushWhileRolling:FlushCallback1");
          TEST_SYNC_POINT(
              "AutoRollLoggerTest::LogFlushWhileRolling:FlushCallback2");
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  flush_thread = std::thread([&]() { auto_roll_logger->Flush(); });
  TEST_SYNC_POINT(
      "AutoRollLoggerTest::LogFlushWhileRolling:PreRollAndPostThreadInit");
  RollLogFileBySizeTest(auto_roll_logger, options.max_log_file_size,
                        kSampleMessage + ":LogFlushWhileRolling");
  flush_thread.join();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

#endif  // OS_WIN

TEST_F(AutoRollLoggerTest, InfoLogLevel) {
  InitTestDb();

  size_t log_size = 8192;
  size_t log_lines = 0;
  // an extra-scope to force the AutoRollLogger to flush the log file when it
  // becomes out of scope.
  {
    AutoRollLogger logger(Env::Default(), kTestDir, "", log_size, 0);
    for (int log_level = InfoLogLevel::HEADER_LEVEL;
         log_level >= InfoLogLevel::DEBUG_LEVEL; log_level--) {
      logger.SetInfoLogLevel((InfoLogLevel)log_level);
      for (int log_type = InfoLogLevel::DEBUG_LEVEL;
           log_type <= InfoLogLevel::HEADER_LEVEL; log_type++) {
        // log messages with log level smaller than log_level will not be
        // logged.
        LogMessage((InfoLogLevel)log_type, &logger, kSampleMessage.c_str());
      }
      log_lines += InfoLogLevel::HEADER_LEVEL - log_level + 1;
    }
    for (int log_level = InfoLogLevel::HEADER_LEVEL;
         log_level >= InfoLogLevel::DEBUG_LEVEL; log_level--) {
      logger.SetInfoLogLevel((InfoLogLevel)log_level);

      // again, messages with level smaller than log_level will not be logged.
      RLOG(InfoLogLevel::HEADER_LEVEL, &logger, "%s", kSampleMessage.c_str());
      RDEBUG(&logger, "%s", kSampleMessage.c_str());
      RINFO(&logger, "%s", kSampleMessage.c_str());
      RWARN(&logger, "%s", kSampleMessage.c_str());
      RERROR(&logger, "%s", kSampleMessage.c_str());
      RFATAL(&logger, "%s", kSampleMessage.c_str());
      log_lines += InfoLogLevel::HEADER_LEVEL - log_level + 1;
    }
  }
  std::ifstream inFile(AutoRollLoggerTest::kLogFile.c_str());
  size_t lines = std::count(std::istreambuf_iterator<char>(inFile),
                         std::istreambuf_iterator<char>(), '\n');
  ASSERT_EQ(log_lines, lines);
  inFile.close();
}

// Test the logger Header function for roll over logs
// We expect the new logs creates as roll over to carry the headers specified
static std::vector<string> GetOldFileNames(const string& path) {
  std::vector<string> ret;

  const string dirname = path.substr(/*start=*/ 0, path.find_last_of("/"));
  const string fname = path.substr(path.find_last_of("/") + 1);

  std::vector<string> children;
  Env::Default()->GetChildren(dirname, &children);

  // We know that the old log files are named [path]<something>
  // Return all entities that match the pattern
  for (auto& child : children) {
    if (fname != child && child.find(fname) == 0) {
      ret.push_back(dirname + "/" + child);
    }
  }

  return ret;
}

// Return the number of lines where a given pattern was found in the file
static size_t GetLinesCount(const string& fname, const string& pattern) {
  std::stringstream ssbuf;
  string line;
  size_t count = 0;

  std::ifstream inFile(fname.c_str());
  ssbuf << inFile.rdbuf();

  while (getline(ssbuf, line)) {
    if (line.find(pattern) != std::string::npos) {
      count++;
    }
  }

  return count;
}

TEST_F(AutoRollLoggerTest, LogHeaderTest) {
  static const size_t MAX_HEADERS = 10;
  static const size_t LOG_MAX_SIZE = 1024 * 5;
  static const std::string HEADER_STR = "Log header line";

  // test_num == 0 -> standard call to Header()
  // test_num == 1 -> call to Log() with InfoLogLevel::HEADER_LEVEL
  for (int test_num = 0; test_num < 2; test_num++) {

    InitTestDb();

    AutoRollLogger logger(Env::Default(), kTestDir, /*db_log_dir=*/ "",
                          LOG_MAX_SIZE, /*log_file_time_to_roll=*/ 0);

    if (test_num == 0) {
      // Log some headers explicitly using Header()
      for (size_t i = 0; i < MAX_HEADERS; i++) {
        RHEADER(&logger, "%s %d", HEADER_STR.c_str(), i);
      }
    } else if (test_num == 1) {
      // HEADER_LEVEL should make this behave like calling Header()
      for (size_t i = 0; i < MAX_HEADERS; i++) {
        RLOG(InfoLogLevel::HEADER_LEVEL, &logger, "%s %d",
            HEADER_STR.c_str(), i);
      }
    }

    const string newfname = logger.TEST_log_fname();

    // Log enough data to cause a roll over
    int i = 0;
    for (size_t iter = 0; iter < 2; iter++) {
      while (logger.GetLogFileSize() < LOG_MAX_SIZE) {
        RINFO(&logger, (kSampleMessage + ":LogHeaderTest line %d").c_str(), i);
        ++i;
      }

      RINFO(&logger, "Rollover");
    }

    // Flush the log for the latest file
    LogFlush(&logger);

    const auto oldfiles = GetOldFileNames(newfname);

    ASSERT_EQ(oldfiles.size(), (size_t) 2);

    for (auto& oldfname : oldfiles) {
      // verify that the files rolled over
      ASSERT_NE(oldfname, newfname);
      // verify that the old log contains all the header logs
      ASSERT_EQ(GetLinesCount(oldfname, HEADER_STR), MAX_HEADERS);
    }
  }
}

TEST_F(AutoRollLoggerTest, LogFileExistence) {
  rocksdb::DB* db;
  rocksdb::Options options;
  string deleteCmd = "rm -rf " + kTestDir;
  ASSERT_EQ(system(deleteCmd.c_str()), 0);
  options.max_log_file_size = 100 * 1024 * 1024;
  options.create_if_missing = true;
  ASSERT_OK(rocksdb::DB::Open(options, kTestDir, &db));
  ASSERT_OK(env->FileExists(kLogFile));
  delete db;
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
