//
// Copyright (C) 2009 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/extent_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <chromeos/make_unique_ptr.h>
#include <chromeos/secure_blob.h>
#include <gtest/gtest.h>

#include "update_engine/payload_constants.h"
#include "update_engine/test_utils.h"
#include "update_engine/utils.h"

using chromeos_update_engine::test_utils::ExpectVectorsEq;
using std::min;
using std::string;
using std::vector;

namespace chromeos_update_engine {

COMPILE_ASSERT(sizeof(off_t) == 8, off_t_not_64_bit);

namespace {
const char kPathTemplate[] = "./ExtentWriterTest-file.XXXXXX";
const size_t kBlockSize = 4096;
}

class ExtentWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memcpy(path_, kPathTemplate, sizeof(kPathTemplate));
    fd_.reset(new EintrSafeFileDescriptor);
    int fd = mkstemp(path_);
    ASSERT_TRUE(fd_->Open(path_, O_RDWR, 0600));
    close(fd);
  }
  void TearDown() override {
    fd_->Close();
    unlink(path_);
  }

  // Writes data to an extent writer in 'chunk_size' chunks with
  // the first chunk of size first_chunk_size. It calculates what the
  // resultant file should look like and ensure that the extent writer
  // wrote the file correctly.
  void WriteAlignedExtents(size_t chunk_size, size_t first_chunk_size);
  void TestZeroPad(bool aligned_size);

  FileDescriptorPtr fd_;
  char path_[sizeof(kPathTemplate)];
};

TEST_F(ExtentWriterTest, SimpleTest) {
  vector<Extent> extents;
  Extent extent;
  extent.set_start_block(1);
  extent.set_num_blocks(1);
  extents.push_back(extent);

  const string bytes = "1234";

  DirectExtentWriter direct_writer;
  EXPECT_TRUE(direct_writer.Init(fd_, extents, kBlockSize));
  EXPECT_TRUE(direct_writer.Write(bytes.data(), bytes.size()));
  EXPECT_TRUE(direct_writer.End());

  EXPECT_EQ(kBlockSize + bytes.size(), utils::FileSize(path_));

  chromeos::Blob result_file;
  EXPECT_TRUE(utils::ReadFile(path_, &result_file));

  chromeos::Blob expected_file(kBlockSize);
  expected_file.insert(expected_file.end(),
                       bytes.data(), bytes.data() + bytes.size());
  ExpectVectorsEq(expected_file, result_file);
}

TEST_F(ExtentWriterTest, ZeroLengthTest) {
  vector<Extent> extents;
  Extent extent;
  extent.set_start_block(1);
  extent.set_num_blocks(1);
  extents.push_back(extent);

  DirectExtentWriter direct_writer;
  EXPECT_TRUE(direct_writer.Init(fd_, extents, kBlockSize));
  EXPECT_TRUE(direct_writer.Write(nullptr, 0));
  EXPECT_TRUE(direct_writer.End());
}

TEST_F(ExtentWriterTest, OverflowExtentTest) {
  WriteAlignedExtents(kBlockSize * 3, kBlockSize * 3);
}

TEST_F(ExtentWriterTest, UnalignedWriteTest) {
  WriteAlignedExtents(7, 7);
}

TEST_F(ExtentWriterTest, LargeUnalignedWriteTest) {
  WriteAlignedExtents(kBlockSize * 2, kBlockSize / 2);
}

void ExtentWriterTest::WriteAlignedExtents(size_t chunk_size,
                                           size_t first_chunk_size) {
  vector<Extent> extents;
  Extent extent;
  extent.set_start_block(1);
  extent.set_num_blocks(1);
  extents.push_back(extent);
  extent.set_start_block(0);
  extent.set_num_blocks(1);
  extents.push_back(extent);
  extent.set_start_block(2);
  extent.set_num_blocks(1);
  extents.push_back(extent);

  chromeos::Blob data(kBlockSize * 3);
  test_utils::FillWithData(&data);

  DirectExtentWriter direct_writer;
  EXPECT_TRUE(direct_writer.Init(fd_, extents, kBlockSize));

  size_t bytes_written = 0;
  while (bytes_written < data.size()) {
    size_t bytes_to_write = min(data.size() - bytes_written, chunk_size);
    if (bytes_written == 0) {
      bytes_to_write = min(data.size() - bytes_written, first_chunk_size);
    }
    EXPECT_TRUE(direct_writer.Write(&data[bytes_written], bytes_to_write));
    bytes_written += bytes_to_write;
  }
  EXPECT_TRUE(direct_writer.End());

  EXPECT_EQ(data.size(), utils::FileSize(path_));

  chromeos::Blob result_file;
  EXPECT_TRUE(utils::ReadFile(path_, &result_file));

  chromeos::Blob expected_file;
  expected_file.insert(expected_file.end(),
                       data.begin() + kBlockSize,
                       data.begin() + kBlockSize * 2);
  expected_file.insert(expected_file.end(),
                       data.begin(), data.begin() + kBlockSize);
  expected_file.insert(expected_file.end(),
                       data.begin() + kBlockSize * 2, data.end());
  ExpectVectorsEq(expected_file, result_file);
}

TEST_F(ExtentWriterTest, ZeroPadNullTest) {
  TestZeroPad(true);
}

TEST_F(ExtentWriterTest, ZeroPadFillTest) {
  TestZeroPad(false);
}

void ExtentWriterTest::TestZeroPad(bool aligned_size) {
  vector<Extent> extents;
  Extent extent;
  extent.set_start_block(1);
  extent.set_num_blocks(1);
  extents.push_back(extent);
  extent.set_start_block(0);
  extent.set_num_blocks(1);
  extents.push_back(extent);

  chromeos::Blob data(kBlockSize * 2);
  test_utils::FillWithData(&data);

  ZeroPadExtentWriter zero_pad_writer(
      chromeos::make_unique_ptr(new DirectExtentWriter()));

  EXPECT_TRUE(zero_pad_writer.Init(fd_, extents, kBlockSize));
  size_t bytes_to_write = data.size();
  const size_t missing_bytes = (aligned_size ? 0 : 9);
  bytes_to_write -= missing_bytes;
  fd_->Seek(kBlockSize - missing_bytes, SEEK_SET);
  EXPECT_EQ(3, fd_->Write("xxx", 3));
  ASSERT_TRUE(zero_pad_writer.Write(data.data(), bytes_to_write));
  EXPECT_TRUE(zero_pad_writer.End());

  EXPECT_EQ(data.size(), utils::FileSize(path_));

  chromeos::Blob result_file;
  EXPECT_TRUE(utils::ReadFile(path_, &result_file));

  chromeos::Blob expected_file;
  expected_file.insert(expected_file.end(),
                       data.begin() + kBlockSize,
                       data.begin() + kBlockSize * 2);
  expected_file.insert(expected_file.end(),
                       data.begin(), data.begin() + kBlockSize);
  if (missing_bytes) {
    memset(&expected_file[kBlockSize - missing_bytes], 0, missing_bytes);
  }

  ExpectVectorsEq(expected_file, result_file);
}

TEST_F(ExtentWriterTest, SparseFileTest) {
  vector<Extent> extents;
  Extent extent;
  extent.set_start_block(1);
  extent.set_num_blocks(1);
  extents.push_back(extent);
  extent.set_start_block(kSparseHole);
  extent.set_num_blocks(2);
  extents.push_back(extent);
  extent.set_start_block(0);
  extent.set_num_blocks(1);
  extents.push_back(extent);
  const int block_count = 4;
  const int on_disk_count = 2;

  chromeos::Blob data(17);
  test_utils::FillWithData(&data);

  DirectExtentWriter direct_writer;
  EXPECT_TRUE(direct_writer.Init(fd_, extents, kBlockSize));

  size_t bytes_written = 0;
  while (bytes_written < (block_count * kBlockSize)) {
    size_t bytes_to_write = min(block_count * kBlockSize - bytes_written,
                                data.size());
    EXPECT_TRUE(direct_writer.Write(data.data(), bytes_to_write));
    bytes_written += bytes_to_write;
  }
  EXPECT_TRUE(direct_writer.End());

  // check file size, then data inside
  ASSERT_EQ(2 * kBlockSize, utils::FileSize(path_));

  chromeos::Blob resultant_data;
  EXPECT_TRUE(utils::ReadFile(path_, &resultant_data));

  // Create expected data
  chromeos::Blob expected_data(on_disk_count * kBlockSize);
  chromeos::Blob big(block_count * kBlockSize);
  for (chromeos::Blob::size_type i = 0; i < big.size(); i++) {
    big[i] = data[i % data.size()];
  }
  memcpy(&expected_data[kBlockSize], &big[0], kBlockSize);
  memcpy(&expected_data[0], &big[3 * kBlockSize], kBlockSize);
  ExpectVectorsEq(expected_data, resultant_data);
}

}  // namespace chromeos_update_engine
