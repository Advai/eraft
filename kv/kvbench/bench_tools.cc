// MIT License

// Copyright (c) 2021 eraft dev group

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <kv/bench_tools.h>
#include <eraftio/kvrpcpb.grpc.pb.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

namespace kvserver {
// BenchTools* BenchTools::instance_ = nullptr;

BenchResult::BenchResult(const std::string& cmd_name, double avg_qps,
                         double avg_latency, uint32_t case_num,
                         double key_num, double valid_key_num)
    : cmd_name_(cmd_name),
      avg_qps_(avg_qps),
      avg_latency_(avg_latency),
      case_num_(case_num),
      key_num_(key_num),
      valid_key_num_(valid_key_num) {}

std::string BenchResult::PrettyBenchResult() const {
  std::string result;
  result += "avg_qps= " + std::to_string(avg_qps_) + "\n";
  result += "avg_latency= " + std::to_string(avg_latency_) + "\n";
  result += "99th_latency = " + std::to_string(key_num_) + "\n";
  result += "99th_qps= " + std::to_string(valid_key_num_) + "\n";
  return result;
}

BenchTools::BenchTools(uint64_t client_nums, uint64_t connection_nums,
                       BenchCmdType cmd_type, uint64_t test_op_count,
                       uint64_t test_key_size_in_bytes,
                       uint64_t test_value_size_in_bytes,
                       std::shared_ptr<RaftClient> raft_client,
                       const std::string& target_addr)
    : client_nums_(client_nums),
      connection_nums_(connection_nums),
      cmd_type_(cmd_type),
      test_op_count_(test_op_count),
      test_key_size_in_bytes_(test_key_size_in_bytes),
      test_value_size_in_bytes_(test_value_size_in_bytes),
      raft_client_(raft_client),
      target_addr_(target_addr) {}

BenchTools::~BenchTools() {}

std::vector<std::pair<std::string, std::string>> BenchTools::GenRandomKvPair(
    uint64_t test_op_count, uint64_t test_key_size_in_bytes,
    uint64_t test_values_size_in_bytes) {
  std::vector<std::pair<std::string, std::string>> test_cases;
  for (uint64_t i = 0; i < test_op_count; i++) {
    test_cases.push_back(std::make_pair<std::string, std::string>(
        GenRandomLenString(test_key_size_in_bytes),
        GenRandomLenString(test_values_size_in_bytes)));
  }
  return test_cases;
}

std::string BenchTools::GenRandomLenString(uint64_t len) {
  const static std::string CHARACTERS =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<> distribution(0, CHARACTERS.size() - 1);

  std::string random_string;

  for (std::size_t i = 0; i < len; ++i) {
    random_string += CHARACTERS[distribution(generator)];
  }
  return random_string;
}


//TODO: Just make this threaded???
BenchResult BenchTools::RunBenchmarks() {
  std::map<std::string, std::string>
      validate_kv;  // evaluate the correctness of raft
  std::vector<std::pair<std::string, std::string>> test_cases =
      GenRandomKvPair(this->test_op_count_, this->test_key_size_in_bytes_,
                      this->test_value_size_in_bytes_);
  std::shared_ptr<Config> conf =
      std::make_shared<Config>(target_addr_, "put", 0);
  std::shared_ptr<RaftClient> raftClient = std::make_shared<RaftClient>(conf);
  kvrpcpb::RawPutRequest put_request;
  put_request.mutable_context()->set_region_id(1);
  put_request.set_cf("test_cf");
  put_request.set_type(1);
  //need to make top precentile vectors
  std::vector<double> _thpt_updates;
  std::vector<double> _latency_updates;
  int update_tick = this->test_op_count_ / 100;
  auto start = std::chrono::system_clock::now();
  auto latest = start;
  int tick = 0;
  for (auto it = test_cases.begin(); it != test_cases.end(); ++it) {
    const std::string& key = it->first;
    const std::string& value = it->second;
    put_request.set_key(key);
    put_request.set_value(value);
    raftClient->PutRaw(this->target_addr_, put_request);
    validate_kv[key] = value;
    // for test, 80ms << 100ms raft tick, we must limit speed to avoid problems
    if (++tick > update_tick){
      auto curr = std::chrono::system_clock::now();
      std::chrono::duration<double> diff = curr - latest;
      double latency = diff.count() / update_tick;
      double qps = update_tick / diff.count();
      latest = curr;
      _thpt_updates.push_back(qps);
      _latency_updates.push_back(latency);
      tick = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto end = std::chrono::system_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  size_t key_num = validate_kv.size();
  size_t valid_key_num = key_num;
  
  //NOTE: Can remove validation for pure speed test..
  /*
  std::cout << "start validation: ------------------------------------- \n";
  std::shared_ptr<Config> conf1 =
      std::make_shared<Config>(target_addr_, "get", 0);
  std::shared_ptr<RaftClient> raftClient1 = std::make_shared<RaftClient>(conf1);

  kvrpcpb::RawGetRequest get_request;
  get_request.mutable_context()->set_region_id(1);
  get_request.set_cf("test_cf");

  size_t key_num = validate_kv.size();
  size_t valid_key_num = 0;
  for (auto it = validate_kv.begin(); it != validate_kv.end(); ++it) {
    const std::string& key = it->first;
    const std::string& value = it->second;

    get_request.set_key(key);
    std::string value_raft =
        raftClient1->GetRaw(this->target_addr_, get_request);

    if (value == value_raft) {
      ++valid_key_num;
    } else {
      std::cout << "ERROR: " << __FILE__ << ' ' << __LINE__
                << " read incorrect value! "
                << " key= " << key << " value= " << value
                << " value_raft= " << value_raft << "\n";
    }
  }
  */
  double avg_latency = elapsed.count() / this->test_op_count_;
  double avg_qps = this->test_op_count_ / elapsed.count();
  std::sort(_thpt_updates.begin(), _thpt_updates.end());
  std::sort(_latency_updates.begin(),  _latency_updates.end());
  double top_one_latency = _latency_updates[98];
  double top_one_throughput = _thpt_updates[0];

  return BenchResult("DEFAULT", avg_qps, avg_latency, this->test_op_count_, top_one_latency,
                     top_one_throughput);
}

}  // namespace kvserver
