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

#include <assert.h>
#include <google/protobuf/text_format.h>
#include <raftcore/raft.h>
#include <raftcore/util.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace eraft {

bool Config::Validate() {
  if (this->id == 0) {
    SPDLOG_ERROR("can't use none as id!");
    return false;
  }
  if (this->heartbeatTick <= 0) {
    SPDLOG_ERROR("log heartbeat tick must be greater than 0!");
    return false;
  }
  if (this->electionTick <= this->heartbeatTick) {
    SPDLOG_ERROR("election tick must be greater than heartbeat tick!");
    return false;
  }
  if (this->storage == nullptr) {
    SPDLOG_ERROR("log storage cannot be nil!");
    return false;
  }
  return true;
}

size_t RaftContext::hash_combine(size_t lhs, size_t rhs) {
  lhs ^= rhs + 0x9e3779b9 + (lhs << 6) + (lhs >> 2);
  return lhs;
}

RaftContext::RaftContext(Config& c)
    : electionElapsed_(0), heartbeatElapsed_(0) {
  assert(c.Validate());
  this->id_ = c.id;
  this->prs_ = std::map<uint64_t, std::shared_ptr<Progress> >{};
  this->prs_b = std::map<uint64_t, std::shared_ptr<Progress> >{};

  this->votes_ = std::map<uint64_t, bool>{};
  this->heartbeatTimeout_ = c.heartbeatTick;
  this->electionTimeout_ = c.electionTick;
  this->raftLog_ = std::make_shared<RaftLog>(c.storage);
  std::tuple<eraftpb::HardState, eraftpb::ConfState> st(
      this->raftLog_->storage_->InitialState());
  eraftpb::HardState hardSt = std::get<0>(st);
  eraftpb::ConfState confSt = std::get<1>(st);
  if (c.peers.size() == 0) {
    std::vector<uint64_t> peersTp;
    for (auto node : confSt.nodes()) {
      peersTp.push_back(node);
    }
    c.peers = peersTp;
  }
  uint64_t lastIndex = this->raftLog_->LastIndex();
  for (auto iter : c.peers) {
    if (iter == this->id_) {
      this->prs_[iter] = std::make_shared<Progress>(lastIndex + 1, lastIndex);
      this->prs_b[iter] = std::make_shared<Progress>(0, 0);
    } else {
      this->prs_[iter] = std::make_shared<Progress>(lastIndex + 1);
      this->prs_b[iter] = std::make_shared<Progress>(0);
    }
  }

  this->BecomeFollower(0, NONE);
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  SPDLOG_INFO("random election timeout is " +
              std::to_string(this->randomElectionTimeout_) + " s");

  this->term_ = hardSt.term();
  this->vote_ = hardSt.vote();
  this->raftLog_->commited_ = hardSt.commit();
  if (c.applied > 0) {
    this->raftLog_->applied_ = c.applied;
  }
  this->leadTransferee_ = NONE;
}

void RaftContext::SendSnapshot(uint64_t to) {
  SPDLOG_INFO("send snap to " + std::to_string(to));
  eraftpb::Snapshot* snapshot = new eraftpb::Snapshot();
  auto snap_ = this->raftLog_->storage_->Snapshot();
  snapshot->mutable_metadata()->set_index(snap_.metadata().index());
  snapshot->mutable_metadata()->set_term(snap_.metadata().term());
  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgSnapshot);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  this->prs_[to]->next = snapshot->metadata().index() + 1;
  msg.set_allocated_snapshot(snapshot);
  this->msgs_.push_back(msg);
}

// DONE
bool RaftContext::SendAppend_b(uint64_t to) {
  
  eraft::ListNode* curr = this->raftLog_->lHead;
  std::vector<eraftpb::Block> blockList;
  for (int i = 0; i < this->prs_b[to]->next; i++) {
    blockList.push_back(curr->block); //list of length prs_b[to]->next size.. (offset to send node i)
    curr = curr->next;
  }
  //Block currBlock = curr->block;
  eraftpb::Block prevBlock = (curr->next)->block;

  if (blockList.size() > 0) {
    eraftpb::BlockMessage msg;
    msg.set_msg_type(eraftpb::MsgAppend);
    msg.set_from(this->id_);
    msg.set_to(to);
    msg.set_term(this->term_);
    msg.set_last_term(this->raftLog_->lastAppendedTerm);
    msg.set_allocated_commit(&this->raftLog_->commitMarker->block);
    msg.set_allocated_prev_block(&prevBlock);
    for (auto it : blockList) {
      eraftpb::Block* e = msg.add_blocks();
      e->set_entry_type(it.entry_type());
      e->set_uid(it.uid());
      e->set_data(it.data());
      SPDLOG_INFO("SET BLOCK DATA =============================" +
                  it.data());
    }  
    this->msgs_b.push_back(msg);
  }
  return true;
}

bool RaftContext::SendAppend(uint64_t to) {
  uint64_t prevIndex = this->prs_[to]->next - 1;
  auto resPair = this->raftLog_->Term(prevIndex);
  uint64_t prevLogTerm = resPair.first;
  if (!resPair.second) {
    // load log from db
    auto entries =
        this->raftLog_->storage_->Entries(prevIndex + 1, prevIndex + 2);
    SPDLOG_INFO("LOAD LOG FROM DB DIZE: " + std::to_string(entries.size()));

    if (entries.size() > 0) {
      prevLogTerm = entries[0].term();
      eraftpb::Message msg;
      msg.set_msg_type(eraftpb::MsgAppend);
      msg.set_from(this->id_);
      msg.set_to(to);
      msg.set_term(this->term_);
      msg.set_commit(this->raftLog_->commited_);
      // msg.set_prev_block(this->raftLog_->chain_.front());
      msg.set_log_term(prevLogTerm);
      // std::vector<uint64> v {
      //    this->id_, to, this->term_, this->raftLog_->commited_, prevIndex
      // };
      // const char* data = reinterpret_cast<const char*>(v.data());
      // std::size_t size = v.size() * sizeof(v[0]);
      // std::hash<std::string_view> hash;
      msg.set_index(prevIndex);
      for (auto ent : entries) {
        eraftpb::Entry* e = msg.add_entries();
        e->set_entry_type(ent.entry_type());
        e->set_index(ent.index());
        e->set_term(ent.term());
        e->set_data(ent.data());
        SPDLOG_INFO("SET ENTRY DATA =============================" +
                    ent.data());
      }
      this->msgs_.push_back(msg);
    }
    return true;
  }

  int64_t n = this->raftLog_->entries_.size();

  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgAppend);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_commit(this->raftLog_->commited_);
  msg.set_log_term(prevLogTerm);
  msg.set_index(prevIndex);
  if (n < 0) {
    n = 0;
  }
  for (uint64_t i = this->raftLog_->ToSliceIndex(prevIndex + 1); i < n; i++) {
    eraftpb::Entry* e = msg.add_entries();
    e->set_entry_type(this->raftLog_->entries_[i].entry_type());
    e->set_index(this->raftLog_->entries_[i].index());
    e->set_term(this->raftLog_->entries_[i].term());
    e->set_data(this->raftLog_->entries_[i].data());
  }
  this->msgs_.push_back(msg);
  return true;
}

//Only need to send who and if the response was rejected..
//Leader will take care of the rest in their response..
// DONE
void RaftContext::SendAppendResponse_b(uint64_t to, bool reject) {
  eraftpb::BlockMessage msg;
  msg.set_msg_type(eraftpb::MsgAppendResponse);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_reject(reject);

  // SPDLOG_INFO("send append response to -> " + std::to_string(to) +
              // " reject -> " + BoolToString(reject) + " term -> " +
              // std::to_string(this->term_);

  this->msgs_b.push_back(msg);

}

void RaftContext::SendAppendResponse(uint64_t to, bool reject, uint64_t term,
                                     uint64_t index) {
  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgAppendResponse);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_reject(reject);
  msg.set_log_term(term);
  msg.set_index(index);

  SPDLOG_INFO("send append response to -> " + std::to_string(to) +
              " reject -> " + BoolToString(reject) + " term -> " +
              std::to_string(term) + " index -> " + std::to_string(index));

  this->msgs_.push_back(msg);
}

void RaftContext::SendHeartbeat(uint64_t to) {
  SPDLOG_INFO("send heartbeat to -> " + std::to_string(to));

  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgHeartbeat);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  this->msgs_.push_back(msg);
}

void RaftContext::SendHeartbeatResponse(uint64_t to, bool reject) {
  SPDLOG_INFO("send heartbeat response to -> " + std::to_string(to) +
              " reject -> " + BoolToString(reject));
  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgHeartbeatResponse);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_reject(reject);
  this->msgs_.push_back(msg);
}

// DONE
void RaftContext::SendRequestVote_b(uint64_t to, uint64_t term) {
  SPDLOG_INFO("send request vote to -> " + std::to_string(to) + " index -> "
      + " term -> " + std::to_string(term));

  eraftpb::BlockMessage msg;
  msg.set_msg_type(eraftpb::MsgRequestVote);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_last_term(term);
  msg.set_allocated_prev_block(&this->raftLog_->lHead->block);
  this->msgs_b.push_back(msg);
}

void RaftContext::SendRequestVote(uint64_t to, uint64_t index, uint64_t term) {
  SPDLOG_INFO("send request vote to -> " + std::to_string(to) + " index -> " +
              std::to_string(index) + " term -> " + std::to_string(term));

  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgRequestVote);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_log_term(term);
  msg.set_index(index);
  this->msgs_.push_back(msg);
}

void RaftContext::SendRequestVoteResponse(uint64_t to, bool reject) {
  SPDLOG_INFO("send request vote response to -> " + std::to_string(to) +
              " reject -> " + BoolToString(reject));

  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgRequestVoteResponse);
  msg.set_from(this->id_);
  msg.set_to(to);
  msg.set_term(this->term_);
  msg.set_reject(reject);
  this->msgs_.push_back(msg);
}

void RaftContext::SendTimeoutNow(uint64_t to) {
  SPDLOG_INFO("timeout now send to -> " + std::to_string(to));

  eraftpb::Message msg;
  msg.set_msg_type(eraftpb::MsgTimeoutNow);
  msg.set_from(this->id_);
  msg.set_to(to);
  this->msgs_.push_back(msg);
}

void RaftContext::Tick() {
  switch (this->state_) {
    case NodeState::StateFollower: {
      this->TickElection();
      break;
    }
    case NodeState::StateCandidate: {
      this->TickElection();
      break;
    }
    case NodeState::StateLeader: {
      if (this->leadTransferee_ != NONE) {
        this->TickTransfer();
      }
      this->TickHeartbeat();
      break;
    }
  }
}

void RaftContext::TickElection() {
  this->electionElapsed_++;
  if (this->electionElapsed_ >=
      this->randomElectionTimeout_) {  // election timeout
    SPDLOG_INFO("election timeout");

    this->electionElapsed_ = 0;
    eraftpb::Message msg;
    msg.set_msg_type(eraftpb::MsgHup);
    this->Step(msg);
  }
}

void RaftContext::TickHeartbeat() {
  this->heartbeatElapsed_++;
  if (this->heartbeatElapsed_ >= this->heartbeatTimeout_) {
    SPDLOG_INFO("heartbeat timeout");
    this->heartbeatElapsed_ = 0;
    eraftpb::Message msg;
    msg.set_msg_type(eraftpb::MsgBeat);
    this->Step(msg);
  }
}

void RaftContext::TickTransfer() {
  this->transferElapsed_++;
  if (this->transferElapsed_ >= this->electionTimeout_ * 2) {
    this->transferElapsed_ = 0;
    this->leadTransferee_ = NONE;
  }
}

void RaftContext::BecomeFollower(uint64_t term, uint64_t lead) {
  this->state_ = NodeState::StateFollower;
  this->lead_ = lead;
  this->term_ = term;
  this->vote_ = NONE;
  SPDLOG_INFO("node become follower at term " + std::to_string(term));
}

void RaftContext::BecomeCandidate() {
  this->state_ = NodeState::StateCandidate;
  this->lead_ = NONE;
  this->term_++;
  this->vote_ = this->id_;
  this->votes_ = std::map<uint64_t, bool>{};
  this->votes_[this->id_] = true;  // vote for self
  SPDLOG_INFO("node become candidate at term " + std::to_string(this->term_));
}

// DONE
void RaftContext::BecomeLeader_b() {
  this->state_ = NodeState::StateLeader;
  this->lead_ = this->id_;
  //uint64_t lastIndex = this->raftLog_->LastIndex();
  this->heartbeatElapsed_ = 0;
  for (auto peer : this->prs_b) {
    if (peer.first == this->id_) {
      //Todo: make nextB, matchB for block only operations
      this->prs_b[peer.first]->next = 0;
      this->prs_b[peer.first]->match = 0;
    } else {
      this->prs_b[peer.first]->next = 0;
    }
  }

  eraftpb::Block blk;
  blk.set_entry_type(eraftpb::EntryNormal);
  blk.set_uid(0);
  blk.set_data("");
  //TODO: Just add ListNode constructor..
  //ListNode* newNode(blk, this->raftLog_->lHead);
  eraft::ListNode* newNode;
  newNode->block = blk;
  newNode->next = this->raftLog_->lHead;
  this->raftLog_->lHead = newNode;
  this->BcastAppend_b();

  if (this->prs_b.size() == 1) {
    //Todo.. if we are the only peer.. can just make commitMarker our head (n/2 exactly has persisited the data.. (??)
    this->raftLog_->commitMarker = newNode;
  } 
  SPDLOG_INFO("node become leader (block) at term " + std::to_string(this->term_));
}

void RaftContext::BecomeLeader() {
  this->state_ = NodeState::StateLeader;
  this->lead_ = this->id_;
  uint64_t lastIndex = this->raftLog_->LastIndex();
  this->heartbeatElapsed_ = 0;
  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      this->prs_[peer.first]->next = lastIndex + 2;
      this->prs_[peer.first]->match = lastIndex + 1;
    } else {
      this->prs_[peer.first]->next = lastIndex + 1;
    }
  }
  eraftpb::Entry ent;
  ent.set_term(this->term_);
  ent.set_index(this->raftLog_->LastIndex() + 1);
  ent.set_entry_type(eraftpb::EntryNormal);
  ent.set_data("");
  this->raftLog_->entries_.push_back(ent);
  this->BcastAppend();
  if (this->prs_.size() == 1) {
    this->raftLog_->commited_ = this->prs_[this->id_]->match;
  }
  SPDLOG_INFO("node become leader at term " + std::to_string(this->term_));
}

bool RaftContext::Step(eraftpb::Message m) {
  if (this->prs_.find(this->id_) == this->prs_.end() &&
      m.msg_type() == eraftpb::MsgTimeoutNow) {
    return false;
  }
  if (m.term() > this->term_) {
    this->leadTransferee_ = NONE;
    this->BecomeFollower(m.term(), NONE);
  }
  switch (this->state_) {
    case NodeState::StateFollower: {
      this->StepFollower(m);
      break;
    }
    case NodeState::StateCandidate: {
      this->StepCandidate(m);
      break;
    }
    case NodeState::StateLeader: {
      this->StepLeader(m);
      break;
    }
  }
  return true;
}

// when follower received message, what to do?
void RaftContext::StepFollower(eraftpb::Message m) {
  switch (m.msg_type()) {
    case eraftpb::MsgHup: {
      this->DoElection();
      break;
    }
    case eraftpb::MsgBeat:
      break;
    case eraftpb::MsgPropose:
      break;
    case eraftpb::MsgAppend: {
      this->HandleAppendEntries(m);
      break;
    }
    case eraftpb::MsgAppendResponse: {
      break;
    }
    case eraftpb::MsgRequestVote: {
      this->HandleRequestVote(m);
      break;
    }
    case eraftpb::MsgRequestVoteResponse:
      break;
    case eraftpb::MsgSnapshot: {
      this->HandleSnapshot(m);
      break;
    }
    case eraftpb::MsgHeartbeat: {
      this->HandleHeartbeat(m);
      break;
    }
    case eraftpb::MsgHeartbeatResponse:
      break;
    case eraftpb::MsgTransferLeader: {
      if (this->lead_ != NONE) {
        m.set_to(this->lead_);
        this->msgs_.push_back(m);
      }
      break;
    }
    case eraftpb::MsgTimeoutNow: {
      this->DoElection();
      break;
    }
    case eraftpb::MsgEntryConfChange: {
      this->msgs_.push_back(m);
      break;
    }
  }
}

void RaftContext::StepCandidate(eraftpb::Message m) {
  switch (m.msg_type()) {
    case eraftpb::MsgHup: {
      this->DoElection();
      break;
    }
    case eraftpb::MsgBeat:
      break;
    case eraftpb::MsgPropose:
      break;
    case eraftpb::MsgAppend: {
      if (m.term() == this->term_) {
        this->BecomeFollower(m.term(), m.from());
      }
      this->HandleAppendEntries(m);
      break;
    }
    case eraftpb::MsgAppendResponse:
      break;
    case eraftpb::MsgRequestVote: {
      this->HandleRequestVote(m);
      break;
    }
    case eraftpb::MsgRequestVoteResponse: {
      this->HandleRequestVoteResponse(m);
      break;
    }
    case eraftpb::MsgSnapshot: {
      this->HandleSnapshot(m);
      break;
    }
    case eraftpb::MsgHeartbeat: {
      if (m.term() == this->term_) {
        this->BecomeFollower(m.term(), m.from());
      }
      this->HandleHeartbeat(m);
      break;
    }
    case eraftpb::MsgHeartbeatResponse:
      break;
    case eraftpb::MsgTransferLeader: {
      if (this->lead_ != NONE) {
        m.set_to(this->lead_);
        this->msgs_.push_back(m);
      }
      break;
    }
    case eraftpb::MsgTimeoutNow:
      break;
    case eraftpb::MsgEntryConfChange: {
      this->msgs_.push_back(m);
      break;
    }
  }
}

void RaftContext::StepLeader(eraftpb::Message m) {
  switch (m.msg_type()) {
    case eraftpb::MsgHup:
      break;
    case eraftpb::MsgBeat: {
      this->BcastHeartbeat();
      break;
    }
    case eraftpb::MsgPropose: {
      if (this->leadTransferee_ == NONE) {
        std::vector<std::shared_ptr<eraftpb::Entry> > entries;
        for (auto ent : m.entries()) {
          auto e = std::make_shared<eraftpb::Entry>(ent);
          entries.push_back(e);
        }
        this->AppendEntries(entries);
      }
      break;
    }
    case eraftpb::MsgAppend: {
      this->HandleAppendEntries(m);
      break;
    }
    case eraftpb::MsgAppendResponse: {
      this->HandleAppendEntriesResponse(m);
      break;
    }
    case eraftpb::MsgRequestVote: {
      this->HandleRequestVote(m);
      break;
    }
    case eraftpb::MsgRequestVoteResponse:
      break;
    case eraftpb::MsgSnapshot: {
      this->HandleSnapshot(m);
      break;
    }
    case eraftpb::MsgHeartbeat: {
      this->HandleHeartbeat(m);
      break;
    }
    case eraftpb::MsgHeartbeatResponse: {
      this->SendAppend(m.from());
      break;
    }
    case eraftpb::MsgTransferLeader: {
      this->HandleTransferLeader(m);
      break;
    }
    case eraftpb::MsgTimeoutNow:
      break;
  }
}

bool RaftContext::DoElection_b() {
  SPDLOG_INFO("node start do election (block)");
  this->BecomeCandidate();
  this->heartbeatElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  if (this->prs_b.size() == 1) {
    this->BecomeLeader();
    return true;
  }

  //Send each peer last log term and head block..
  uint64_t lastLogTerm = this->raftLog_->lastAppendedTerm;
  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      continue;
    }
    this->SendRequestVote_b(peer.first, lastLogTerm);
  }
}

bool RaftContext::DoElection() {
  SPDLOG_INFO("node start do election");

  this->BecomeCandidate();
  this->heartbeatElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  if (this->prs_.size() == 1) {
    this->BecomeLeader();
    return true;
  }
  uint64_t lastIndex = this->raftLog_->LastIndex();
  auto resPair = this->raftLog_->Term(lastIndex);
  uint64_t lastLogTerm = resPair.first;

  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      continue;
    }
    this->SendRequestVote(peer.first, lastIndex, lastLogTerm);
  }
}

void RaftContext::BcastHeartbeat() {
  SPDLOG_INFO("bcast heartbeat ");
  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      continue;
    }
    SPDLOG_INFO("send heartbeat to peer " + std::to_string(peer.first));
    this->SendHeartbeat(peer.first);
  }
}

// DONE
void RaftContext::BcastAppend_b() {
  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      continue;
    }
    this->SendAppend_b(peer.first);
  }
}

void RaftContext::BcastAppend() {
  for (auto peer : this->prs_) {
    if (peer.first == this->id_) {
      continue;
    }
    this->SendAppend(peer.first);
  }
}

// DONE
bool RaftContext::HandleRequestVote_b(eraftpb::BlockMessage m) {
  
  SPDLOG_INFO("handle request vote (block) from " + std::to_string(m.from()));
  // reject if message term not set or less than curr term
  if (m.term() != NONE && m.term() < this->term_) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  // reject if voted already
  if (this->vote_ != NONE && this->vote_ != m.from()) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  // m.index() = last appended term of candidate here
  // reject if follower has greater last appended term
  if (m.last_term() > this->raftLog_->lastAppendedTerm) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  // reject if candidate not up to date
  bool candidateUptoDate = false;
  if (m.last_term() == this->raftLog_->lastAppendedTerm) {
    ListNode* followerCurr = this->raftLog_->WalkBackN(this->raftLog_->lHead, this->prs_b[m.to()]->next);
    ListNode* candidateCurr = this->raftLog_->WalkBackN(this->raftLog_->lHead, this->prs_b[m.from()]->next);
    if (followerCurr->block == candidateCurr->block) {
      // candidate and follower heads are same
      candidateUptoDate = true;
    } else {
      // candidate's head extends follower's head
      while (candidateCurr != nullptr) {
        if candidateCurr->block == followerCurr->block) {
          candidateUptoDate = true;
          break;
        } 
        candidateCurr = candidateCurr->next;
      }
    }
  }
  if (!candidateUptoDate) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  this->vote_ = m.from();
  this->electionElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  this->SendRequestVoteResponse(m.from(), false);
}

bool RaftContext::HandleRequestVote(eraftpb::Message m) {
  SPDLOG_INFO("handle request vote from " + std::to_string(m.from()));
  if (m.term() != NONE && m.term() < this->term_) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  // ...
  if (this->vote_ != NONE && this->vote_ != m.from()) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  uint64_t lastIndex = this->raftLog_->LastIndex();

  auto resPair = this->raftLog_->Term(lastIndex);
  uint64_t lastLogTerm = resPair.first;

  if (lastLogTerm > m.log_term() ||
      (lastLogTerm == m.log_term() && lastIndex > m.index())) {
    this->SendRequestVoteResponse(m.from(), true);
    return true;
  }
  this->vote_ = m.from();
  this->electionElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  this->SendRequestVoteResponse(m.from(), false);
}

// DONE
bool RaftContext::HandleRequestVoteResponse_b(eraftpb::BlockMessage m) {
  SPDLOG_INFO("handle request vote response from peer " +
              std::to_string(m.from()));
  if (m.term() != NONE && m.term() < this->term_) {
    this->SendRequestVoteResponse(m.from(), true);
    return false;
  }

  this->votes_[m.from()] = !m.reject();
  uint8_t grant = 0;
  uint8_t votes = this->votes_.size();
  uint8_t threshold = this->prs_.size() / 2;
  for (auto vote : this->votes_) {
    if (vote.second) {
      grant++;
    }
  }
  if (grant > threshold) {
    this->BecomeLeader_b();
  } else if (votes - grant > threshold) {
    this->BecomeFollower(this->term_, NONE);
  }
}

bool RaftContext::HandleRequestVoteResponse(eraftpb::Message m) {
  SPDLOG_INFO("handle request vote response from peer " +
              std::to_string(m.from()));
  if (m.term() != NONE && m.term() < this->term_) {
    return false;
  }
  this->votes_[m.from()] = !m.reject();
  uint8_t grant = 0;
  uint8_t votes = this->votes_.size();
  uint8_t threshold = this->prs_.size() / 2;
  for (auto vote : this->votes_) {
    if (vote.second) {
      grant++;
    }
  }
  if (grant > threshold) {
    this->BecomeLeader();
  } else if (votes - grant > threshold) {
    this->BecomeFollower(this->term_, NONE);
  }
}

// DONE
bool RaftContext::HandleAppendEntries_b(eraftpb::BlockMessage m) {
  // SPDLOG_INFO("handle append entries (block) " + MessageToString(m));

  if (m.term() != NONE && m.term() < this->term_) {
    this->SendAppendResponse_b(m.from(), true);
    return false;
  }
  
  this->electionElapsed_ = 0;
  this->randomElectionTimeout_ = this->electionTimeout_ + RandIntn(this->electionTimeout_);
  this->lead_ = m.from();

  ListNode* prev = this->raftLog_->FindBlock(m.prev_block());
  // 
  if (!prev) {
    this->SendAppendResponse_b(m.from(), true);
    return false;
  }

  //Update chain/head
  for (auto block : m.blocks()) {
    ListNode* newnode;
    newnode->next = prev;
    newnode->block = block;
    this->raftLog_->lHead = newnode;
    prev = newnode;
  }
  this->raftLog_->lastAppendedTerm = this->term_;
  //Send response rpc
  this->SendAppendResponse_b(m.from(), false);
  return true;
  }

bool RaftContext::HandleAppendEntries(eraftpb::Message m) {
  SPDLOG_INFO("handle append entries " + MessageToString(m));

  // return false if term of msg < current term
  if (m.term() != NONE && m.term() < this->term_) {
    this->SendAppendResponse(m.from(), true, NONE, NONE);
    return false;
  }
  
  this->electionElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  this->lead_ = m.from();
  // this->raftLog_->commitMarker = m.commit();
  uint64_t lastIndex = this->raftLog_->LastIndex();

  // if (this->raftLog_->entries_[lastIndex] != m.prev_block()) {
  //   this->SendAppendReponse(m.from(), true, NONE, lastIndex + 1);
  // }
  
  // if msg index > last index, reject to get 
  if (m.index() > lastIndex) {
    this->SendAppendResponse(m.from(), true, NONE, lastIndex + 1);
    return false;
  }
  // if log doesn't contain entry at prevLogIndex whose term matches prevLogTerm
  if (m.index() >= this->raftLog_->firstIndex_) {
    auto resPair = this->raftLog_->Term(m.index());
    uint64_t logTerm = resPair.first;
    if (logTerm != m.log_term()) {
      uint64_t index = 0;
      for (uint64_t i = 0; i < this->raftLog_->ToSliceIndex(m.index() + 1);
           i++) {
        if (this->raftLog_->entries_[i].term() == logTerm) {
          index = i;
        }
      }
      this->SendAppendResponse(m.from(), true, logTerm, index);
      return false;
    }
  }
  uint64_t count = 0;
  for (auto entry : m.entries()) {
    if (entry.index() < this->raftLog_->firstIndex_) {
      continue;
    }
    if (entry.index() <= this->raftLog_->LastIndex()) {
      auto resPair = this->raftLog_->Term(entry.index());
      uint64_t logTerm = resPair.first;
      if (logTerm != entry.term()) {
        uint64_t idx = this->raftLog_->ToSliceIndex(entry.index());
        this->raftLog_->entries_[idx] = entry;
        this->raftLog_->entries_.erase(
            this->raftLog_->entries_.begin() + idx + 1,
            this->raftLog_->entries_.end());
        this->raftLog_->stabled_ =
            std::min(this->raftLog_->stabled_, entry.index() - 1);
      }
    } else {
      uint64_t n = m.entries().size();
      for (uint64_t j = count; j < n; j++) {
        this->raftLog_->entries_.push_back(m.entries(j));
      }
      break;
    }
    count++;
  }
  if (m.commit() > this->raftLog_->commited_) {
    this->raftLog_->commited_ =
        std::min(m.commit(), m.index() + m.entries().size());
  }
  this->SendAppendResponse(m.from(), false, NONE, this->raftLog_->LastIndex());
}

// DONE
bool RaftContext::HandleAppendEntriesResponse_b(eraftpb::BlockMessage m) {
  // DONE
  SPDLOG_INFO("handle append entries (block) response from" + std::to_string(m.from()));
  if (m.term() != NONE && m.term() < this->term_) {
    return false;
  }
  
  if (m.reject()) {
    //Message was rejected.. 
    this->prs_[m.from()]->next++; //increment next to send the next block..
    this->SendAppend(m.from());  
    return false;
  }
  //message was accepted..
  this->prs_[m.from()]->next = 0; //update nextoffset to 0
  this->prs_[m.from()]->match = 0; //Matches up to our head..
  this->LeaderCommit_b(); //TODO: update LeaderCommit to use blocks..
  if (m.from() == this->leadTransferee_ && m.term() == this->raftLog_->lastAppendedTerm) {
    this->SendTimeoutNow(m.from()); 
    this->leadTransferee_ = NONE;
  }
  
  return true;
}

void RaftContext::LeaderCommit_b() {
  return;
}

bool RaftContext::HandleAppendEntriesResponse(eraftpb::Message m) {
  SPDLOG_INFO("handle append entries response from" + std::to_string(m.from()));
  if (m.term() != NONE && m.term() < this->term_) {
    return false;
  }
  if (m.reject()) {
    uint64_t index = m.index();
    if (index == NONE) {
      return false;
    }
    if (m.log_term() != NONE) {
      uint64_t logTerm = m.log_term();
      uint64_t fIndex;
      for (uint64_t i = 0; i < this->raftLog_->entries_.size(); i++) {
        if (this->raftLog_->entries_[i].term() > logTerm) {
          fIndex = i;
        }
      }
      if (fIndex > 0 &&
          this->raftLog_->entries_[fIndex - 1].term() == logTerm) {
        index = this->raftLog_->ToEntryIndex(fIndex);
      }
    }
    this->prs_[m.from()]->next = index;
    this->SendAppend(m.from());
    return false;
  }
  if (m.index() > this->prs_[m.from()]->match) {
    this->prs_[m.from()]->match = m.index();
    this->prs_[m.from()]->next = m.index() + 1;
    this->LeaderCommit();
    if (m.from() == this->leadTransferee_ &&
        m.index() == this->raftLog_->LastIndex()) {
      this->SendTimeoutNow(m.from());
      this->leadTransferee_ = NONE;
    }
  }
}

void RaftContext::LeaderCommit() {
  std::vector<uint64_t> match;
  match.resize(this->prs_.size());
  uint64_t i = 0;
  for (auto prs : this->prs_) {
    match[i] = prs.second->match;
    i++;
  }
  std::sort(match.begin(), match.end());
  uint64_t n = match[(this->prs_.size() - 1) / 2];
  if (n > this->raftLog_->commited_) {
    auto resPair = this->raftLog_->Term(n);
    uint64_t logTerm = resPair.first;
    if (logTerm == this->term_) {
      // commit 条件，半数节点以上
      this->raftLog_->commited_ = n;
      this->BcastAppend();
    }
  }
  SPDLOG_INFO("leader commit on log index " +
              std::to_string(this->raftLog_->commited_));
}

bool RaftContext::HandleHeartbeat(eraftpb::Message m) {
  SPDLOG_INFO("raft_context::handle_heart_beat");
  if (m.term() != NONE && m.term() < this->term_) {
    this->SendHeartbeatResponse(m.from(), true);
    return false;
  }
  this->lead_ = m.from();
  this->electionElapsed_ = 0;
  this->randomElectionTimeout_ =
      this->electionTimeout_ + RandIntn(this->electionTimeout_);
  this->SendHeartbeatResponse(m.from(), false);
}

void RaftContext::AppendEntries_b(std::vector<std::shared_ptr<eraftpb::Block>> blocks) {
  SPDLOG_INFO("append blocks size " + std::to_string(blocks.size()));
  // for (auto block: blocks) {
  //   block->set_term(this->term_);
  //   if (entry->entry_type() == eraftpb::EntryConfChange) {
      
  //   }
  // }
}

void RaftContext::AppendEntries(
    std::vector<std::shared_ptr<eraftpb::Entry> > entries) {
  SPDLOG_INFO("append entries size " + std::to_string(entries.size()));
  uint64_t lastIndex = this->raftLog_->LastIndex();
  uint64_t i = 0;
  // push entries to raftLog
  for (auto entry : entries) {
    entry->set_term(this->term_);
    entry->set_index(lastIndex + i + 1);
    if (entry->entry_type() == eraftpb::EntryConfChange) {
      if (this->pendingConfIndex_ != NONE) {
        continue;
      }
      this->pendingConfIndex_ = entry->index();
    }
    this->raftLog_->entries_.push_back(*entry);
  }
  this->prs_[this->id_]->match = this->raftLog_->LastIndex();
  this->prs_[this->id_]->next = this->prs_[this->id_]->match + 1;
  this->BcastAppend();
  if (this->prs_.size() == 1) {
    this->raftLog_->commited_ = this->prs_[this->id_]->match;
  }
}

std::shared_ptr<ESoftState> RaftContext::SoftState() {
  return std::make_shared<ESoftState>(this->lead_, this->state_);
}

std::shared_ptr<eraftpb::HardState> RaftContext::HardState() {
  std::shared_ptr<eraftpb::HardState> hd =
      std::make_shared<eraftpb::HardState>();
  hd->set_term(this->term_);
  hd->set_vote(this->vote_);
  hd->set_commit(this->raftLog_->commited_);
  return hd;
}

bool RaftContext::HandleSnapshot(eraftpb::Message m) {
  SPDLOG_INFO("handle snapshot from: " + std::to_string(m.from()));
  eraftpb::SnapshotMetadata meta = m.snapshot().metadata();
  if (meta.index() <= this->raftLog_->commited_) {
    this->SendAppendResponse(m.from(), false, NONE, this->raftLog_->commited_);
    return false;
  }
  this->BecomeFollower(std::max(this->term_, m.term()), m.from());
  uint64_t first = meta.index() + 1;
  if (this->raftLog_->entries_.size() > 0) {
    this->raftLog_->entries_.clear();
  }
  this->raftLog_->firstIndex_ = first;
  this->raftLog_->applied_ = meta.index();
  this->raftLog_->commited_ = meta.index();
  this->raftLog_->stabled_ = meta.index();
  for (auto peer : meta.conf_state().nodes()) {
    this->prs_[peer] = std::make_shared<Progress>();
  }
  this->raftLog_->pendingSnapshot_ = m.snapshot();
  this->SendAppendResponse(m.from(), false, NONE, this->raftLog_->LastIndex());
}

bool RaftContext::HandleTransferLeader(eraftpb::Message m) {
  if (m.from() == this->id_) {
    return false;
  }
  if (this->leadTransferee_ != NONE && this->leadTransferee_ == m.from()) {
    return false;
  }
  if (this->prs_[m.from()] == nullptr) {
    return false;
  }
  this->leadTransferee_ = m.from();
  this->transferElapsed_ = 0;
  if (this->prs_[m.from()]->match == this->raftLog_->LastIndex()) {
    this->SendTimeoutNow(m.from());
  } else {
    this->SendAppend(m.from());
  }
}

void RaftContext::AddNode(uint64_t id) {
  SPDLOG_INFO("add node id " + std::to_string(id));

  if (this->prs_[id] == nullptr) {
    this->prs_[id] = std::make_shared<Progress>(6);
  }
  this->pendingConfIndex_ = NONE;
}

void RaftContext::RemoveNode(uint64_t id) {
  SPDLOG_INFO("remove node id " + std::to_string(id));

  if (this->prs_[id] != nullptr) {
    this->prs_.erase(id);
    if (this->state_ == NodeState::StateLeader) {
      this->LeaderCommit();
    }
  }
  this->pendingConfIndex_ = NONE;
}

std::vector<eraftpb::Message> RaftContext::ReadMessage() {
  std::vector<eraftpb::Message> msgs = this->msgs_;
  this->msgs_.clear();
  return msgs;
}

}  // namespace eraft
