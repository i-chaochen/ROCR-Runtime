/******************************************************************************

Copyright �2013 Advanced Micro Devices, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#include "os.h"
#include "helper_funcs.hpp"
#include "hsa_rsrc_factory.hpp"
#include "test_hsa.h"

bool TestHSA::initialize(int arg_cnt, char** arg_list) {
  std::cout << "TestHSA::initialize :" << std::endl;
  // Initialize command line arguments
  hsa_cmdline_arg_cnt = arg_cnt;
  hsa_cmdline_arg_list = arg_list;

  // Instantiate a Timer object
  setup_timer_idx_ = hsa_timer_.CreateTimer();
  dispatch_timer_idx_ = hsa_timer_.CreateTimer();

  // Instantiate an instance of Hsa Resources Factory
  hsa_rsrc_ = new HsaRsrcFactory();

  // Print properties of the agents
  hsa_rsrc_->PrintGpuAgents("> GPU agents");

  // Create an instance of Gpu agent
  const char* p = getenv("ROCR_AGENT_IND");
  const uint32_t agent_ind = (p == NULL) ? 0 : atol(p);
  if (!hsa_rsrc_->GetGpuAgentInfo(agent_ind, &agent_info_)) {
    std::cout << "> error: agent[" << agent_ind << "] is not found" << std::endl;
    return false;
  }
  std::cout << "> Using agent[" << agent_ind << "] : " << agent_info_->name << std::endl;

  // Create an instance of Aql Queue
  uint32_t num_pkts = 128;
  hsa_rsrc_->CreateQueue(agent_info_, num_pkts, &hsa_queue_);

  // Obtain handle of signal
  hsa_rsrc_->CreateSignal(1, &hsa_signal_);

  // Obtain the code object file name
  std::string agentName(agent_info_->name);
  if (agentName.compare(0, 4, "gfx8") == 0) {
    brig_path_obj_.append("gfx8");
  } else if (agentName.compare(0, 4, "gfx9") == 0) {
    brig_path_obj_.append("gfx9");
  } else {
    assert(false);
    return false;
  }
  brig_path_obj_.append("_" + name_ + ".hsaco");

  return true;
}

bool TestHSA::setup() {
  std::cout << "TestHSA::setup :" << std::endl;

  // Start the timer object
  hsa_timer_.StartTimer(setup_timer_idx_);

  mem_map_t& mem_map = test_->get_mem_map();
  for (mem_it_t it = mem_map.begin(); it != mem_map.end(); ++it) {
    mem_descr_t& des = it->second;
    void* ptr = (des.local) ? hsa_rsrc_->AllocateLocalMemory(agent_info_, des.size)
                            : hsa_rsrc_->AllocateSysMemory(agent_info_, des.size);
    des.ptr = ptr;
    assert(ptr != NULL);
    if (ptr == NULL) return false;
  }
  test_->init();

  // Load and Finalize Kernel Code Descriptor
  char* brig_path = (char*)brig_path_obj_.c_str();
  const bool ret_val =
      hsa_rsrc_->LoadAndFinalize(agent_info_, brig_path, strdup(name_.c_str()), &kernel_code_desc_);
  if (ret_val == false) {
    std::cout << "Error in loading and finalizing Kernel" << std::endl;
    return ret_val;
  }

  // Stop the timer object
  hsa_timer_.StopTimer(setup_timer_idx_);
  setup_time_taken_ = hsa_timer_.ReadTimer(setup_timer_idx_);
  total_time_taken_ = setup_time_taken_;

  return true;
}

bool TestHSA::run() {
  std::cout << "TestHSA::run :" << std::endl;

  const uint32_t work_group_size = 64;
  const uint32_t work_grid_size = test_->get_elements_count();
  uint32_t group_segment_size = 0;
  uint32_t private_segment_size = 0;
  const size_t kernarg_segment_size = test_->get_kernarg_size();
  uint64_t code_handle = 0;

  // Retrieve the amount of group memory needed
  hsa_executable_symbol_get_info(
      kernel_code_desc_, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE, &group_segment_size);

  // Retrieve the amount of private memory needed
  hsa_executable_symbol_get_info(kernel_code_desc_,
                                 HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,
                                 &private_segment_size);

  // Check the kernel args size
  size_t size_info = 0;
  hsa_executable_symbol_get_info(
      kernel_code_desc_, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &size_info);
  assert(kernarg_segment_size == size_info);
  if (kernarg_segment_size != size_info) return false;

  // Retrieve handle of the code block
  hsa_executable_symbol_get_info(kernel_code_desc_, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
                                 &code_handle);

  // Initialize the dispatch packet.
  hsa_kernel_dispatch_packet_t aql;
  memset(&aql, 0, sizeof(aql));
  // Set the packet's type, barrier bit, acquire and release fences
  aql.header = HSA_PACKET_TYPE_KERNEL_DISPATCH;
  aql.header |= 1ul << HSA_PACKET_HEADER_BARRIER;
  aql.header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE;
  aql.header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE;
  // Populate Aql packet with default values
  aql.setup = 1;
  aql.grid_size_x = work_grid_size;
  aql.grid_size_y = 1;
  aql.grid_size_z = 1;
  aql.workgroup_size_x = work_group_size;
  aql.workgroup_size_y = 1;
  aql.workgroup_size_z = 1;
  // Bind the kernel code descriptor and arguments
  aql.kernel_object = code_handle;
  aql.kernarg_address = test_->get_kernarg_ptr();
  aql.group_segment_size = group_segment_size;
  aql.private_segment_size = private_segment_size;
  // Initialize Aql packet with handle of signal
  aql.completion_signal = hsa_signal_;

  // Compute the write index of queue and copy Aql packet into it
  const uint64_t que_idx = hsa_queue_load_write_index_relaxed(hsa_queue_);
  const uint32_t mask = hsa_queue_->size - 1;

  std::cout << "> Executing kernel: \"" << name_ << "\"" << std::endl;

  // Start the timer object
  hsa_timer_.StartTimer(dispatch_timer_idx_);

  // Disable packet so that submission to HW is complete
  const auto header = aql.header;
  const uint8_t packet_type_mask = (1 << HSA_PACKET_HEADER_WIDTH_TYPE) - 1;
  aql.header &= (~packet_type_mask) << HSA_PACKET_HEADER_TYPE;
  aql.header |= HSA_PACKET_TYPE_INVALID << HSA_PACKET_HEADER_TYPE;

  // Copy Aql packet into queue buffer
  ((hsa_kernel_dispatch_packet_t*)(hsa_queue_->base_address))[que_idx & mask] = aql;

  // After AQL packet is fully copied into queue buffer
  // update packet header from invalid state to valid state
  std::atomic_thread_fence(std::memory_order_release);
  ((hsa_kernel_dispatch_packet_t*)(hsa_queue_->base_address))[que_idx & mask].header = header;

  // Increment the write index and ring the doorbell to dispatch the kernel.
  hsa_queue_store_write_index_relaxed(hsa_queue_, (que_idx + 1));
  hsa_signal_store_relaxed(hsa_queue_->doorbell_signal, que_idx);

  std::cout << "> Waiting on kernel dispatch signal" << std::endl;

  // Wait on the dispatch signal until the kernel is finished.
  // Update wait condition to HSA_WAIT_STATE_ACTIVE for Polling
  hsa_signal_value_t value = hsa_signal_wait_acquire(hsa_signal_, HSA_SIGNAL_CONDITION_LT, 1,
                                                     (uint64_t)-1, HSA_WAIT_STATE_BLOCKED);

  // Stop the timer object
  hsa_timer_.StopTimer(dispatch_timer_idx_);
  dispatch_time_taken_ = hsa_timer_.ReadTimer(dispatch_timer_idx_);
  total_time_taken_ += dispatch_time_taken_;

  // Copy kernel buffers from local memory into system memory
  hsa_rsrc_->TransferData((uint8_t*)test_->get_output_ptr(), (uint8_t*)test_->get_local_ptr(),
                          test_->get_output_size(), false);
  test_->print_output();

  return true;
}

bool TestHSA::verify_results() {
  // Compare the results and see if they match
  const int32_t cmp_val =
      memcmp(test_->get_output_ptr(), test_->get_refout_ptr(), test_->get_output_size());
  return (cmp_val == 0);
}

void TestHSA::print_time() {
  std::cout << "Time taken for Setup by " << this->name_ << " : " << this->setup_time_taken_
            << std::endl;
  std::cout << "Time taken for Dispatch by " << this->name_ << " : " << this->dispatch_time_taken_
            << std::endl;
  std::cout << "Time taken in Total by " << this->name_ << " : " << this->total_time_taken_
            << std::endl;
}

bool TestHSA::cleanup() {
  // shutdown Hsa Runtime system
  hsa_status_t ret_val = hsa_shut_down();
  return (HSA_STATUS_SUCCESS == ret_val);
}