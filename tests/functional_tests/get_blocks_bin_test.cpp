// Copyright (c) 2014-2026, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <iostream>
#include <boost/program_options.hpp>

#include "include_base_utils.h"
#include "common/command_line.h"
#include "common/util.h"
#include "net/http.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "storages/http_abstract_invoke.h"

namespace po = boost::program_options;
using namespace epee;

static const std::chrono::seconds RPC_TIMEOUT = std::chrono::minutes(3) + std::chrono::seconds(30);

namespace
{
  const command_line::arg_descriptor<std::string> arg_daemon_addr = {"daemon-addr", "", "127.0.0.1:18081"};
  const command_line::arg_descriptor<uint64_t> arg_start_block = {"start_block", "", 0};
  const command_line::arg_descriptor<uint64_t> arg_block_count = {"block_count", "", 10};
}

struct test_result
{
  uint64_t bytes_received = 0;
  size_t blocks_received = 0;
  size_t total_tx_blobs_size = 0;
  size_t total_block_size = 0;
  bool success = false;
  std::string error_message;
};

test_result run_get_blocks_test(const std::string& daemon_addr, uint64_t start_block, uint64_t block_count, bool get_tx_blobs)
{
  test_result result;
  
  LOG_PRINT_L0("Creating HTTP client for get_tx_blobs=" << (get_tx_blobs ? "true" : "false"));
  
  net::http::client_factory factory;
  auto http_client = factory.create();

  if (!http_client->set_server(daemon_addr, boost::none))
  {
    result.error_message = "Failed to set server";
    LOG_ERROR(result.error_message);
    return result;
  }

  if (!http_client->connect(RPC_TIMEOUT))
  {
    result.error_message = "Failed to connect to daemon";
    LOG_ERROR(result.error_message);
    return result;
  }

  LOG_PRINT_L0("Connected, getting hashes from height " << start_block);

  // Get hashes - use get_blocks.bin directly which supports get_tx_blobs
  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::request req = AUTO_VAL_INIT(req);
  req.start_height = start_block;
  req.max_block_count = block_count;
  req.get_tx_blobs = get_tx_blobs;
  
  // For gethashes.bin, we need to provide at least one block ID as starting point
  // Use genesis block hash for Monero mainnet
  req.block_ids.push_back(crypto::null_hash);

  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::response res = AUTO_VAL_INIT(res);

  LOG_PRINT_L0("Requesting " << block_count << " blocks from height " << start_block << " with get_tx_blobs=" << (get_tx_blobs ? "true" : "false"));

  uint64_t bytes_before = http_client->get_bytes_received();
  bool ok = net_utils::invoke_http_bin("/getblocks.bin", req, res, *http_client, RPC_TIMEOUT);
  uint64_t bytes_after = http_client->get_bytes_received();

  if (!ok)
  {
    result.error_message = "Failed to get blocks (invoke_http_bin returned false)";
    LOG_ERROR(result.error_message);
    return result;
  }

  result.bytes_received = bytes_after - bytes_before;
  result.blocks_received = res.blocks.size();
  result.success = true;

  // Calculate data sizes
  for (const auto& block : res.blocks)
  {
    result.total_block_size += block.block.size();
    if (get_tx_blobs)
    {
      for (const auto& tx : block.txs)
      {
        result.total_tx_blobs_size += tx.blob.size();
      }
    }
  }

  LOG_PRINT_L0("Got " << result.blocks_received << " blocks, " << result.bytes_received << " bytes");
  
  http_client->disconnect();
  return result;
}

int main(int argc, char *argv[])
{
  TRY_ENTRY();

  tools::on_startup();
  mlog_configure(mlog_get_default_log_path("get_blocks_bin_test.log"), true);
  mlog_set_log_level(2);

  po::options_description desc_options("Allowed options");
  command_line::add_arg(desc_options, command_line::arg_help);
  command_line::add_arg(desc_options, arg_daemon_addr);
  command_line::add_arg(desc_options, arg_start_block);
  command_line::add_arg(desc_options, arg_block_count);

  po::variables_map vm;
  bool r = command_line::handle_error_helper(desc_options, [&]()
  {
    po::store(po::parse_command_line(argc, argv, desc_options), vm);
    po::notify(vm);
    return true;
  });
  if (!r)
    return 1;

  if (command_line::get_arg(vm, command_line::arg_help))
  {
    std::cout << desc_options << std::endl;
    return 0;
  }

  std::string daemon_addr = command_line::get_arg(vm, arg_daemon_addr);
  uint64_t start_block = command_line::get_arg(vm, arg_start_block);
  uint64_t block_count = command_line::get_arg(vm, arg_block_count);

  LOG_PRINT_L0("Connecting to daemon at " << daemon_addr);
  LOG_PRINT_L0("Starting from block " << start_block << ", fetching up to " << block_count << " blocks");

  // Get current height first to validate start_block
  {
    net::http::client_factory factory;
    auto temp_client = factory.create();

    if (!temp_client->set_server(daemon_addr, boost::none))
    {
      LOG_ERROR("Failed to set server: " << daemon_addr);
      return 1;
    }

    if (!temp_client->connect(RPC_TIMEOUT))
    {
      LOG_ERROR("Failed to connect to daemon");
      return 1;
    }

    cryptonote::COMMAND_RPC_GET_HEIGHT::request height_req = AUTO_VAL_INIT(height_req);
    cryptonote::COMMAND_RPC_GET_HEIGHT::response height_res = AUTO_VAL_INIT(height_res);
    bool height_ok = net_utils::invoke_http_json("/get_height", height_req, height_res, *temp_client, RPC_TIMEOUT);
    if (!height_ok)
    {
      LOG_ERROR("Failed to get blockchain height");
      return 1;
    }

    uint64_t current_height = height_res.height;
    LOG_PRINT_L0("Current blockchain height: " << current_height);

    if (start_block >= current_height)
    {
      LOG_ERROR("Start block " << start_block << " is beyond current height " << current_height);
      return 1;
    }
    
    temp_client->disconnect();
  }

  LOG_PRINT_L0("\n=== Running bandwidth comparison test (sequential) ===");

  test_result result_with_blobs = run_get_blocks_test(daemon_addr, start_block, block_count, true);
  test_result result_no_blobs = run_get_blocks_test(daemon_addr, start_block, block_count, false);

  if (!result_with_blobs.success)
  {
    LOG_ERROR("Test with get_tx_blobs=true failed: " << result_with_blobs.error_message);
    return 1;
  }

  if (!result_no_blobs.success)
  {
    LOG_ERROR("Test with get_tx_blobs=false failed: " << result_no_blobs.error_message);
    return 1;
  }

  LOG_PRINT_L0("\n=== Test with get_tx_blobs = true ===");
  LOG_PRINT_L0("Bytes received: " << result_with_blobs.bytes_received);
  LOG_PRINT_L0("Blocks received: " << result_with_blobs.blocks_received);
  LOG_PRINT_L0("Block data: " << result_with_blobs.total_block_size << " bytes");
  LOG_PRINT_L0("Tx blob data: " << result_with_blobs.total_tx_blobs_size << " bytes");

  LOG_PRINT_L0("\n=== Test with get_tx_blobs = false ===");
  LOG_PRINT_L0("Bytes received: " << result_no_blobs.bytes_received);
  LOG_PRINT_L0("Blocks received: " << result_no_blobs.blocks_received);

  LOG_PRINT_L0("\n=== Bandwidth Comparison Summary ===");
  LOG_PRINT_L0("With tx_blobs:    " << result_with_blobs.bytes_received << " bytes");
  LOG_PRINT_L0("Without tx_blobs: " << result_no_blobs.bytes_received << " bytes");
  
  if (result_with_blobs.bytes_received > result_no_blobs.bytes_received)
  {
    uint64_t savings = result_with_blobs.bytes_received - result_no_blobs.bytes_received;
    double reduction = (100.0 * savings) / result_with_blobs.bytes_received;
    LOG_PRINT_L0("Savings:          " << savings << " bytes");
    LOG_PRINT_L0("Reduction:        " << reduction << "%");
  }
  else if (result_no_blobs.bytes_received > result_with_blobs.bytes_received)
  {
    uint64_t increase = result_no_blobs.bytes_received - result_with_blobs.bytes_received;
    double ratio = (100.0 * increase) / result_no_blobs.bytes_received;
    LOG_PRINT_L0("Increase:         " << increase << " bytes");
    LOG_PRINT_L0("Increase ratio:   " << ratio << "%");
  }
  else
  {
    LOG_PRINT_L0("No difference in bandwidth");
  }

  CATCH_ENTRY_L0("main", 1);

  return 0;
}
