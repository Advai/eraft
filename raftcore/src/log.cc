// Copyright 2015 The etcd Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// MIT License

// Copyright (c) 2021 Colin

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
#include <raftcore/log.h>
#include <raftcore/util.h>
#include <spdlog/spdlog.h>

#include <algorithm>

// RaftLog manage the log entries, its struct look like:
//
//  snapshot/first.....applied....committed....stabled.....last
//  --------|------------------------------------------------|
//                            log entries
//
// for simplify the RaftLog implement should manage all log entries
// that not truncated

namespace eraft {
// newLog returns log using the given storage. It recovers the log
// to the state that it just commits and applies the latest snapshot.
RaftLog::RaftLog(std::shared_ptr<StorageInterface> st) {
  uint64_t lo = st->FirstIndex();
  uint64_t hi = st->LastIndex();
  std::vector<eraftpb::Entry> entries = st->Entries(lo, hi + 1);
  this->storage_ = st;
  this->entries_ = entries;
  this->applied_ = lo - 1;
  this->stabled_ = hi;
  this->firstIndex_ = lo;

  eraftpb::Block genesis;
  genesis.set_data("GENESIS");
  genesis.set_entry_type(eraftpb::EntryNormal);
  genesis.set_uid(0);
  // blk.set_uid(uuids::to_string(uuids::uuid_system_gnerator{}()));
  // init list node and pushback to vec
  //std::vector<std::shared_ptr<ListNode>> cHead;
  std::shared_ptr<ListNode> GenNode;
  GenNode->block = genesis;
  GenNode->next = std::shared_ptr<ListNode>{};
  this->_Genesis = GenNode; 
  //vec.push_back(GenNode);
  //this->cHeads_ = vec;
  this->hTails_[GenNode] = GenNode; //need to start our build
  this->lHead = GenNode; 
  this->commited_marker = GenNode;
  SPDLOG_INFO("init raft log with firstIndex " +
              std::to_string(this->firstIndex_) + " applied " +
              std::to_string(this->applied_) + " stabled " +
              std::to_string(this->stabled_) + " commited " +
              std::to_string(this->commited_));
}

RaftLog::~RaftLog() {}

// We need to compact the log entries in some point of time like
// storage compact stabled log entries prevent the log entries
// grow unlimitedly in memory
void RaftLog::MaybeCompact() {
  uint64_t first = this->storage_->FirstIndex();
  if (first > this->firstIndex_) {
    if (this->entries_.size() > 0) {
      this->entries_.erase(this->entries_.begin(),
                           this->entries_.begin() + this->ToSliceIndex(first));
    }
    this->firstIndex_ = first;
  }
}

std::vector<eraftpb::Entry> RaftLog::UnstableEntries() {
  if (this->entries_.size() > 0) {
    return std::vector<eraftpb::Entry>(
        this->entries_.begin() + (this->stabled_ - this->firstIndex_ + 1),
        this->entries_.end());
  }
  return std::vector<eraftpb::Entry>{};
}

std::vector<eraftpb::Entry> RaftLog::NextEnts() {
  SPDLOG_INFO("raftlog next ents l.applied: " + std::to_string(this->applied_) +
              " l.firstIndex: " + std::to_string(this->firstIndex_) +
              " l.commited: " + std::to_string(this->commited_));

  if (this->entries_.size() > 0) {
    return std::vector<eraftpb::Entry>(
        this->entries_.begin() + (this->applied_ - this->firstIndex_ + 1),
        this->entries_.begin() + this->commited_ - this->firstIndex_ + 1);
  }
  return std::vector<eraftpb::Entry>{};
}

uint64_t RaftLog::ToSliceIndex(uint64_t i) {
  uint64_t idx = i - this->firstIndex_;
  if (idx < 0) {
    return 0;
  }
  return idx;
}

uint64_t RaftLog::ToEntryIndex(uint64_t i) { return i + this->firstIndex_; }

uint64_t RaftLog::LastIndex() {
  uint64_t index = 0;
  // if (!IsEmptySnap(pendingSnapshot_)) {
  //   index = pendingSnapshot_.metadata().index();
  // }
  if (this->entries_.size() > 0) {
    return std::max(this->entries_[this->entries_.size() - 1].index(), index);
  }
  uint64_t i = this->storage_->LastIndex();
  return std::max(i, index);
}

std::pair<uint64_t, bool> RaftLog::Term(uint64_t i) {
  if (this->entries_.size() > 0 && i >= this->firstIndex_) {
    return std::make_pair<uint64_t, bool>(
        this->entries_[i - this->firstIndex_].term(), true);
  }
  // i <= firstIndex
  std::pair<uint64_t, bool> pair = this->storage_->Term(i);
  if (!pair.second) {
    return std::make_pair<uint64_t, bool>(0, false);
  }
  uint64_t term_ = pair.first;
  if (term_ == 0 && !IsEmptySnap(pendingSnapshot_)) {
    if (i == pendingSnapshot_.metadata().index()) {
      term_ = static_cast<uint64_t>(pendingSnapshot_.metadata().index());
    }
  }
  return std::make_pair<uint64_t, bool>(static_cast<uint64_t>(term_), true);
}

/* 
    Chained Log Interface Implementations:
    Remove Side Chains (garbage collection)
    Helper Functions
    Compare Block abstraction layer
*/

bool RaftLog::isSameBlock(static eraftpb::Block& block1, static eraft::Block& block2){
  return ((block1.data() == block2.data()) && (block1.uid() == block2.uid()) && (block1.entry_type() == block2.entry_type()));
}

void RaftLog::RemoveSideChains() {
  auto block = this->commitMarker->next->block;
}

std::shared_ptr<ListNode> RaftLog::WalkBackN(ListNode* head, int n) {
  ListNode* curr = head;
  for (int i = 0; i < n; i++) {
    curr = curr->next;
  }
  return curr;
}


//args: block-> block to find  Return: ListNode of block on success, or nullptr on failure
std::shared_ptr<ListNode> RaftLog::FindBlock(eraftpb::Block block) {
  for (auto it : this->hTails_) { //check all side chains..(using map..makes more sense)
    std::shared_ptr<ListNode> ret = FindBlockFrom(block, this->hTails_[it.first]);
    if (ret != nullptr){
      //finegrained log for tracing
      SPDLOG_INFO("raftlog: found block " + std::to_string(block.uid()) +
              " from leader");
      return ret;
    }
  }
  return nullptr;
  /*
  for (int i = 0; i < this->cHeads_.size(); i++){
    auto node_ptr = FindBlockFrom(block, this->cHeads_[i]);
    if (node_ptr != nullptr){
      return node_ptr;
    }
  }
  */
}

//TODO: Find more greedy "Marker" to track how far we need to search from
std::shared_ptr<ListNode> RaftLog::FindBlockFrom(eraftpb::Block block, std::shared_ptr<ListNode> head) {
  std::shared_ptr<ListNode> temp = head;
  while (!isSameBlock(temp->block, block)){
    if (temp == this->_Genesis || temp->next = nullptr) {
      return nullptr;
    }
    temp = temp->next;
  }
  return temp;
}

uint64_t RaftLog::HeadtoCommitOffset() {
  std::shared_ptr temp = this->lHead;
  uint64_t off = 0;
  while (temp != this->commited_marker) {
    if (temp == this->Genesis_){
      SPDLOG_INFO("raftlog: bad lookup when tracing from head to commit marker..");
      return off;
    }
    off++;
    temp = temp->next;
  }
  return off;
}

//TODO: Not sure where to start. IS this even needed?
void RaftLog::TryCompactB(){
  return;
}

void RaftLog::FreeChain(std::shared_ptr<ListNode> head, std::shared_ptr<ListNode> tail) {
  while (head->next != tail  || !head->next == nullptr) {
    std::shared_ptr<ListNode> temp = head;
    head = head->next;
    if (this->hTails_.find(temp) != this->hTails_.end()){
      this->hTails_.remove(temp);
    }
    free(temp->block);
    delete(temp); //should do automatically 
  }
}

void RaftLog::CommitGarbageCollect(uint8_t n) {
  //args n: blocks to garbage collect extending forks
  uint8_t collected = 0;
  std::shared_ptr<ListNode> tail = this->commited_marker;
  for (int i = 0; i < n; ++i) {
    std::shared_ptr<ListNode> tail = tail->next;
    for (auto it : this->hTails_) {
      if (it.second == tail && it.first != this->commited_marker) { //tail is node that directly extends the commit chain
        collected++; //can garbage collect..delete pointers and update structures
        this->FreeChain(it.first, it.second);
        this->hTails_.remove(it.first);
        //this->bMap[it.first.] -> just do inside free function
      }
    }
  }
  SPDLOG_INFO("raftlog garbage collect on commit: " + std::to_string(this->commited_) +
              " l.prev_commit: " + std::to_string(this->commited_ - n) +
              " l.applied: " + std::to_string(this->applied_) +
              " collected: " + std::to_string(collected));
}
}  // namespace eraft
