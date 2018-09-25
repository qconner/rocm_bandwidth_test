////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
// 
// Copyright (c) 2014-2015, Advanced Micro Devices, Inc. All rights reserved.
// 
// Developed by:
// 
//                 AMD Research and AMD HSA Software Development
// 
//                 Advanced Micro Devices, Inc.
// 
//                 www.amd.com
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include "common.hpp"
#include "rocm_bandwidth_test.hpp"

#include <stdlib.h>
#include <assert.h>
#include <algorithm>
#include <unistd.h>
#include <cctype>
#include <sstream>
#include <limits>

// The values are in megabytes at allocation time
const uint32_t RocmBandwidthTest::SIZE_LIST[] = { 1 * 1024,
                                 2 * 1024, 4 * 1024, 8 * 1024,
                                 16 * 1024, 32 * 1024, 64 * 1024,
                                 128 * 1024, 256 * 1024, 512 * 1024,
                                 1 * 1024 * 1024, 2 * 1024 * 1024,
                                 4 * 1024 * 1024, 8 * 1024 * 1024,
                                 16 * 1024 * 1024, 32 * 1024 * 1024,
                                 64 * 1024 * 1024, 128 * 1024 * 1024,
                                 256 * 1024 * 1024, 512 * 1024 * 1024 };

uint32_t RocmBandwidthTest::GetIterationNum() {
  return (validate_) ? 1 : (num_iteration_ * 1.2 + 1);
}

void RocmBandwidthTest::AcquireAccess(hsa_agent_t agent, void* ptr) {
  err_ = hsa_amd_agents_allow_access(1, &agent, NULL, ptr);
  ErrorCheck(err_);
}

void RocmBandwidthTest::AcquirePoolAcceses(uint32_t src_dev_idx,
                                   hsa_agent_t src_agent, void* src,
                                   uint32_t dst_dev_idx,
                                   hsa_agent_t dst_agent, void* dst) {

  if (access_matrix_[(src_dev_idx * agent_index_) + dst_dev_idx] == 2) {
    AcquireAccess(src_agent, dst);
    AcquireAccess(dst_agent, src);
    return;
  }

  // determine which one is a cpu and call acquire on the other agent
  hsa_device_type_t src_dev_type = agent_list_[src_dev_idx].device_type_;
  hsa_device_type_t dst_dev_type = agent_list_[dst_dev_idx].device_type_;
  if (src_dev_type == HSA_DEVICE_TYPE_CPU) {
    AcquireAccess(dst_agent, src);
    return;
  }
  if (dst_dev_type == HSA_DEVICE_TYPE_CPU) {
    AcquireAccess(src_agent, dst);
    return;
  }
  assert(false && "Inconsistent state");
}

void RocmBandwidthTest::AllocateHostBuffers(uint32_t size,
                                    uint32_t src_dev_idx,
                                    uint32_t dst_dev_idx,
                                    void*& src, void*& dst,
                                    void* buf_src, void* buf_dst,
                                    hsa_agent_t src_agent, hsa_agent_t dst_agent,
                                    hsa_signal_t& signal) {

  // Allocate host buffers and setup accessibility for copy operation
  err_ = hsa_amd_memory_pool_allocate(sys_pool_, size, 0, (void**)&src);
  ErrorCheck(err_);

  // Gain access to the pools
  AcquirePoolAcceses(cpu_index_, cpu_agent_, src,
                     src_dev_idx, src_agent, buf_src);

  err_ = hsa_amd_memory_pool_allocate(sys_pool_, size, 0, (void**)&dst);
  ErrorCheck(err_);

  // Gain access to the pools
  AcquirePoolAcceses(dst_dev_idx, dst_agent, buf_dst,
                     cpu_index_, cpu_agent_, dst);

  // Initialize host buffers to a determinate value
  memset(src, 0x23, size);
  memset(dst, 0x00, size);
  
  // Create a signal to wait on copy operation
  // @TODO: replace it with a signal pool call
  err_ = hsa_signal_create(1, 0, NULL, &signal);
  ErrorCheck(err_);

  return;
}

void RocmBandwidthTest::AllocateCopyBuffers(uint32_t size,
                        uint32_t src_dev_idx, uint32_t dst_dev_idx,
                        void*& src, hsa_amd_memory_pool_t src_pool,
                        void*& dst, hsa_amd_memory_pool_t dst_pool,
                        hsa_agent_t src_agent, hsa_agent_t dst_agent,
                        hsa_signal_t& signal) {

  // Allocate buffers in src and dst pools for forward copy
  err_ = hsa_amd_memory_pool_allocate(src_pool, size, 0, &src);
  ErrorCheck(err_);
  err_ = hsa_amd_memory_pool_allocate(dst_pool, size, 0, &dst);
  ErrorCheck(err_);

  // Create a signal to wait on copy operation
  // @TODO: replace it with a signal pool call
  err_ = hsa_signal_create(1, 0, NULL, &signal);
  ErrorCheck(err_);

  return AcquirePoolAcceses(src_dev_idx, src_agent, src,
                            dst_dev_idx, dst_agent, dst);
}

void RocmBandwidthTest::ReleaseBuffers(bool bidir,
                               void* src_fwd, void* src_rev,
                               void* dst_fwd, void* dst_rev,
                               hsa_signal_t signal_fwd,
                               hsa_signal_t signal_rev) {

  // Free the src and dst buffers used in forward copy
  // including the signal used to wait
  err_ = hsa_amd_memory_pool_free(src_fwd);
  ErrorCheck(err_);
  err_ = hsa_amd_memory_pool_free(dst_fwd);
  ErrorCheck(err_);
  err_ = hsa_signal_destroy(signal_fwd);
  ErrorCheck(err_);

  // Free the src and dst buffers used in reverse copy
  // including the signal used to wait
  if (bidir) {
    err_ = hsa_amd_memory_pool_free(src_rev);
    ErrorCheck(err_);
    err_ = hsa_amd_memory_pool_free(dst_rev);
    ErrorCheck(err_);
    err_ = hsa_signal_destroy(signal_rev);
    ErrorCheck(err_);
  }
}

double RocmBandwidthTest::GetGpuCopyTime(bool bidir,
                                 hsa_signal_t signal_fwd,
                                 hsa_signal_t signal_rev) {

  // Obtain time taken for forward copy
  hsa_amd_profiling_async_copy_time_t async_time_fwd = {0};
  err_= hsa_amd_profiling_get_async_copy_time(signal_fwd, &async_time_fwd);
  ErrorCheck(err_);
  if (bidir == false) {
    return(async_time_fwd.end - async_time_fwd.start);
  }

  hsa_amd_profiling_async_copy_time_t async_time_rev = {0};
  err_= hsa_amd_profiling_get_async_copy_time(signal_rev, &async_time_rev);
  ErrorCheck(err_);
  double start = min(async_time_fwd.start, async_time_rev.start);
  double end = max(async_time_fwd.end, async_time_rev.end);
  return(end - start);
}

void RocmBandwidthTest::copy_buffer(void* dst, hsa_agent_t dst_agent,
                            void* src, hsa_agent_t src_agent,
                            size_t size, hsa_signal_t signal) {

  // Copy from src into dst buffer
  err_ = hsa_amd_memory_async_copy(dst, dst_agent,
                                   src, src_agent,
                                   size, 0, NULL, signal);
  ErrorCheck(err_);

  // Wait for the forward copy operation to complete
  while (hsa_signal_wait_acquire(signal, HSA_SIGNAL_CONDITION_LT, 1,
                                     uint64_t(-1), HSA_WAIT_STATE_ACTIVE));
}

void RocmBandwidthTest::RunCopyBenchmark(async_trans_t& trans) {

  // Bind if this transaction is bidirectional
  bool bidir = trans.copy.bidir_;

  // Initialize size of buffer to equal the largest element of allocation
  uint32_t size_len = size_list_.size();
  uint32_t max_size = size_list_.back();

  // Bind to resources such as pool and agents that are involved
  // in both forward and reverse copy operations
  void* buf_src_fwd;
  void* buf_dst_fwd;
  void* buf_src_rev;
  void* buf_dst_rev;
  void* validation_dst;
  void* validation_src;
  hsa_signal_t signal_fwd;
  hsa_signal_t signal_rev;
  hsa_signal_t validation_signal;
  uint32_t src_idx = trans.copy.src_idx_;
  uint32_t dst_idx = trans.copy.dst_idx_;
  uint32_t src_dev_idx_fwd = pool_list_[src_idx].agent_index_;
  uint32_t dst_dev_idx_fwd = pool_list_[dst_idx].agent_index_;
  uint32_t src_dev_idx_rev = dst_dev_idx_fwd;
  uint32_t dst_dev_idx_rev = src_dev_idx_fwd;
  hsa_amd_memory_pool_t src_pool_fwd = trans.copy.src_pool_;
  hsa_amd_memory_pool_t dst_pool_fwd = trans.copy.dst_pool_;
  hsa_amd_memory_pool_t src_pool_rev = dst_pool_fwd;
  hsa_amd_memory_pool_t dst_pool_rev = src_pool_fwd;
  hsa_agent_t src_agent_fwd = pool_list_[src_idx].owner_agent_;
  hsa_agent_t dst_agent_fwd = pool_list_[dst_idx].owner_agent_;
  hsa_agent_t src_agent_rev = dst_agent_fwd;
  hsa_agent_t dst_agent_rev = src_agent_fwd;

  // Allocate buffers and signal objects
  AllocateCopyBuffers(max_size,
                      src_dev_idx_fwd,
                      dst_dev_idx_fwd,
                      buf_src_fwd, src_pool_fwd,
                      buf_dst_fwd, dst_pool_fwd,
                      src_agent_fwd, dst_agent_fwd,
                      signal_fwd);

  if (bidir) {
    AllocateCopyBuffers(max_size,
                        src_dev_idx_rev,
                        dst_dev_idx_rev,
                        buf_src_rev, src_pool_rev,
                        buf_dst_rev, dst_pool_rev,
                        src_agent_rev, dst_agent_rev,
                        signal_rev);
  }

  if (validate_) {
    AllocateHostBuffers(max_size,
                        src_dev_idx_fwd,
                        dst_dev_idx_fwd,
                        validation_src, validation_dst,
                        buf_src_fwd, buf_dst_fwd,
                        src_agent_fwd, dst_agent_fwd,
                        validation_signal);

    // Initialize source buffer with values from verification buffer
    copy_buffer(buf_src_fwd, src_agent_fwd,
                validation_src, cpu_agent_,
                max_size, validation_signal);
  }

  // Bind the number of iterations
  uint32_t iterations = GetIterationNum();

  // TODO: promote iterations count to a command line argument
  //iterations = 1000; // temporary override

  // Iterate through the different buffer sizes to
  // compute the bandwidth as determined by copy
  for (uint32_t idx = 0; idx < size_len; idx++) {
    
    // This should not be happening
    uint32_t curr_size = size_list_[idx];
    if (curr_size > max_size) {
      cerr << "ERROR: Illegal DMA buffer size" << endl;
      break;
    }

    // verify == false means the verification option was turned on but the data changed after DMA'ing it around
    bool verify = true;

    cout << endl << "RUNNING " << iterations << " ITERATIONS for buffer size " << curr_size << endl;

    // this is an accumulator for elapsed GPU time to conduct a DMA
    double accumulated_gpu_time = 0.0;

    // Create a timer object and reset signals
    PerfTimer timer;
    uint32_t index = timer.CreateTimer();

    // Start the CPU-side time
    timer.StartTimer(index);

    // run a number of iterations for this DMA buffer size
    for (uint32_t it = 0; it < iterations; it++) {
      /*
      if (it % 2) {
        printf(".");
        fflush(stdout);
      }
      */
      cout << ".";

      hsa_signal_store_relaxed(signal_fwd, 1);
      if (bidir) {
        hsa_signal_store_relaxed(signal_rev, 1);
      }

      if (validate_) { 
        AcquirePoolAcceses(src_dev_idx_fwd,
                           src_agent_fwd, buf_src_fwd,
                           dst_dev_idx_fwd,
                           dst_agent_fwd, buf_dst_fwd);
      }

      // Launch forward copy operation
      err_ = hsa_amd_memory_async_copy(buf_dst_fwd, dst_agent_fwd,
                                       buf_src_fwd, src_agent_fwd,
                                       curr_size, 0, NULL, signal_fwd);
      ErrorCheck(err_);

      // Launch reverse copy operation if it is bidirectional
      if (bidir) {
        err_ = hsa_amd_memory_async_copy(buf_dst_rev, dst_agent_rev,
                                         buf_src_rev, src_agent_rev,
                                         curr_size, 0, NULL, signal_rev);
        ErrorCheck(err_);
      }

      if (bw_blocking_run_ == NULL) {
        cout << "F";
        // Wait for the forward copy operation to complete
        while (hsa_signal_wait_acquire(signal_fwd, HSA_SIGNAL_CONDITION_LT, 1,
                                       uint64_t(-1), HSA_WAIT_STATE_ACTIVE));

        // Wait for the reverse copy operation to complete
        if (bidir) {
	  cout << "R";
          while (hsa_signal_wait_acquire(signal_rev, HSA_SIGNAL_CONDITION_LT, 1,
                                         uint64_t(-1), HSA_WAIT_STATE_ACTIVE));
        }

      } else {

        // Wait for the forward copy operation to complete
	cout << "f";
	hsa_signal_wait_acquire(signal_fwd, HSA_SIGNAL_CONDITION_LT, 1,
                                       uint64_t(-1), HSA_WAIT_STATE_BLOCKED);

        // Wait for the reverse copy operation to complete
	cout << "r";
        if (bidir) {
          hsa_signal_wait_acquire(signal_rev, HSA_SIGNAL_CONDITION_LT, 1,
                                         uint64_t(-1), HSA_WAIT_STATE_BLOCKED);
        }

      }

      if (validate_) {
        cout << "V";
	cout.flush();
        // Re-Establish access to destination buffer and host buffer
        AcquirePoolAcceses(dst_dev_idx_fwd,
                           dst_agent_fwd, buf_dst_fwd,
                           cpu_index_, cpu_agent_, validation_dst);

        // Init dst buffer with values from outbuffer of copy operation
        hsa_signal_store_relaxed(validation_signal, 1);
        copy_buffer(validation_dst, cpu_agent_,
                    buf_dst_fwd, dst_agent_fwd,
                    curr_size, validation_signal);

        // Compare output equals input
        err_ = (hsa_status_t)memcmp(validation_src, validation_dst, curr_size);
        if (err_ != HSA_STATUS_SUCCESS) {
          verify = false;
          exit_value_ = err_;
	  cerr << "ERROR: data corrupted during DMA" << endl;
	  break;
        }
      }
      
      // Collect time from the signal(s)
      if (print_cpu_time_ == false) {
	if (trans.copy.uses_gpu_) {
	  accumulated_gpu_time += GetGpuCopyTime(bidir, signal_fwd, signal_rev);
	}
      }

    }  // end iterations

    // Stop the timer object
    timer.StopTimer(index);

    cout << endl << "ran " << iterations << " iterations" << endl;

    // aggregate elapsed time for "iterations" count of DMA transfers
    double aggregate_cpu_time = timer.ReadTimer(index);

    // per-DMA timing is not practical b/c the libC call to get time of day
    // takes longer than the DMA itself, limiting the ability of the benchmark
    // to saturate the PCIe bus

    // for this reason, min copy time will equal mean copy time for both the cpu and gpu

    cout << endl;
    cout << "USING CPU TSC TIMER:" << endl;
    cout << "elapsed seconds:     " << aggregate_cpu_time << endl;
    cout << "seconds per DMA:     " << aggregate_cpu_time / iterations << endl;
    cout << "agg BW (GB/sec):     " << ((double)curr_size / aggregate_cpu_time) * ((double)iterations / (double)(1024 * 1024 * 1024)) << endl;  // watch for integer overflow


    // Get Cpu min copy time
    trans.cpu_min_time_.push_back(aggregate_cpu_time / (double)iterations);  // elapsed time for single buffer
                                                                           // since Display() thinks only curr_size bytes were sent
                                                                           // Display() doesn't know about iterations

    // Get Cpu mean copy time and store to the array
    trans.cpu_avg_time_.push_back(aggregate_cpu_time / (double)iterations);

    if (print_cpu_time_ == false) {
      if (trans.copy.uses_gpu_) {
	// Get Gpu min and mean copy times
	cout << endl << "USING GPU COPY TIMES:" << endl;
	cout << "elapsed seconds:     " << (double)accumulated_gpu_time / 1E9 << endl;
        cout << "seconds per DMA:     " << (double)accumulated_gpu_time / 1E9 / iterations << endl;
        cout << "agg BW (GB/sec):     " << ((double)curr_size * iterations / (double)(1024 * 1024 * 1024) /
					    ((double)accumulated_gpu_time / 1E9)) << endl;  // watch for integer overflow

        double min_time = (verify) ? accumulated_gpu_time  / (double)iterations: std::numeric_limits<double>::max();
	double mean_time = (verify) ? accumulated_gpu_time  / (double)iterations: std::numeric_limits<double>::max();
	trans.gpu_min_time_.push_back(min_time);
	trans.gpu_avg_time_.push_back(mean_time);
      }
    }

  }


  // Free up buffers and signal objects used in copy operation
  ReleaseBuffers(bidir, buf_src_fwd, buf_src_rev,
                 buf_dst_fwd, buf_dst_rev, signal_fwd, signal_rev);

  if (validate_) {
    hsa_signal_t fake_signal = {0};
    ReleaseBuffers(false, validation_src, NULL,
                   validation_dst, NULL, validation_signal, fake_signal);
  }
}

void RocmBandwidthTest::Run() {

  // Enable profiling of Async Copy Activity
  if (print_cpu_time_ == false) {
    err_ = hsa_amd_profiling_async_copy_enable(true);
    ErrorCheck(err_);
  }

  // Iterate through the list of transactions and execute them
  uint32_t trans_size = trans_list_.size();
  for (uint32_t idx = 0; idx < trans_size; idx++) {
    async_trans_t& trans = trans_list_[idx];
    if ((trans.req_type_ == REQ_COPY_BIDIR) ||
        (trans.req_type_ == REQ_COPY_UNIDIR) ||
        (trans.req_type_ == REQ_COPY_ALL_BIDIR) ||
        (trans.req_type_ == REQ_COPY_ALL_UNIDIR)) {
      RunCopyBenchmark(trans);
      ComputeCopyTime(trans);
    }
    if ((trans.req_type_ == REQ_READ) ||
        (trans.req_type_ == REQ_WRITE)) {
      RunIOBenchmark(trans);
    }
  }
  std::cout << std::endl;

  // Disable profiling of Async Copy Activity
  if (print_cpu_time_ == false) {
    err_ = hsa_amd_profiling_async_copy_enable(false);
    ErrorCheck(err_);
  }

}

void RocmBandwidthTest::Close() {
  hsa_status_t status = hsa_shut_down();
  ErrorCheck(status);
  return;
}

// Sets up the bandwidth test object to enable running
// the various test scenarios requested by user. The
// things this proceedure takes care of are:
//    
//    Parse user arguments
//    Discover RocR Device Topology
//    Determine validity of requested test scenarios
//    Build the list of transactions to execute
//    Miscellaneous
//
void RocmBandwidthTest::SetUp() {

  // Parse user arguments
  ParseArguments();

  // Validate input parameters
  bool status = ValidateArguments();
  if (status == false) {
    PrintHelpScreen();
    exit(1);
  }

  // Build list of transactions (copy, read, write) to execute
  status = BuildTransList();
  if (status == false) {
    PrintHelpScreen();
    exit(1);
  }
}

RocmBandwidthTest::RocmBandwidthTest(int argc, char** argv, size_t num) : BaseTest(num) {

  usr_argc_ = argc;
  usr_argv_ = argv;

  pool_index_ = 0;
  cpu_index_ = -1;
  agent_index_ = 0;
  
  req_read_ = REQ_INVALID;
  req_write_ = REQ_INVALID;
  req_copy_bidir_ = REQ_INVALID;
  req_copy_unidir_ = REQ_INVALID;
  req_copy_all_bidir_ = REQ_INVALID;
  req_copy_all_unidir_ = REQ_INVALID;
  
  link_matrix_ = NULL;
  access_matrix_ = NULL;
  active_agents_list_ = NULL;
  
  validate_ = false;
  print_cpu_time_ = false;

  // Initialize version of the test
  version_.major_id = 1;
  version_.minor_id = 0;
  version_.step_id = 0;
  version_.reserved = 0;

  bw_default_run_ = getenv("ROCM_BW_DEFAULT_RUN");
  bw_blocking_run_ = getenv("ROCR_BW_RUN_BLOCKING");
  skip_fine_grain_ = getenv("ROCM_SKIP_FINE_GRAINED_POOL");

  exit_value_ = 0;
}

RocmBandwidthTest::~RocmBandwidthTest() { }

std::string RocmBandwidthTest::GetVersion() const {

  std::stringstream stream;
  stream << version_.major_id << ".";
  stream << version_.minor_id << ".";
  stream << version_.step_id;
  return stream.str();
}

