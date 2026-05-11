/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "platform/consensus/ordering/raft/algorithm/raft.h"

#include <execinfo.h>
#include <fcntl.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include "chain/storage/storage.h"
#include "common/crypto/signature_verifier.h"
#include "common/utils/utils.h"
#include "platform/consensus/ordering/raft/proto/proposal.pb.h"
#include "platform/proto/resdb.pb.h"

namespace resdb {
namespace raft {

void PrintStackTrace() {
  void* buffer[64];
  int n = backtrace(buffer, 64);
  char** symbols = backtrace_symbols(buffer, n);

  for (int i = 0; i < n; ++i) {
    LOG(INFO) << symbols[i];
  }

  free(symbols);
}

std::ostream& operator<<(std::ostream& stream, Role role) {
  const char* name_role[] = {"FOLLOWER", "CANDIDATE", "LEADER"};
  return stream << name_role[static_cast<int>(role)];
}

std::ostream& operator<<(std::ostream& stream, TermRelation tr) {
  const char* name_term_relation[] = {"STALE", "CURRENT", "NEW"};
  return stream << name_term_relation[static_cast<int>(tr)];
}

uint32_t LogEntry::GetSerializedSize() const {
  if (serialized_size == 0) {
    serialized_size = ComputeSerializedEntrySize();
  }
  return serialized_size;
}

uint32_t LogEntry::ComputeSerializedEntrySize() const {
  return entry.ByteSizeLong();
}

Raft::Raft(int id, int f, int total_num, SignatureVerifier* verifier,
           LeaderElectionManager* leader_election_manager,
           ReplicaCommunicator* replica_communicator, RaftRecovery* recovery)
    : ProtocolBase(id, f, total_num),
      current_term_(0),
      voted_for_(-1),
      last_log_index_(-1),  // This value is unsigned, but after the sentinel is
                            // added wraps back around to 0.
      commit_index_(0),
      last_committed_(0),
      role_(Role::FOLLOWER),
      snapshot_last_index_(0),
      snapshot_last_term_(0),
      heartbeats_sent_this_term_(0),
      is_stop_(false),
      quorum_((total_num / 2) + 1),
      verifier_(verifier),
      leader_election_manager_(leader_election_manager),
      replica_communicator_(replica_communicator),
      recovery_(recovery) {
  assert(recovery_);
  id_ = id;
  total_num_ = total_num;
  f_ = (total_num - 1) / 2;

  // Derive snapshot file paths from the same directory as the WAL/metadata.
  // recovery_->GetFilePath() returns e.g. "./wal_log/log", so we use its
  // parent directory.
  {
    std::string wal_dir = recovery_->GetWalDir();
    snapshot_file_path_ = wal_dir + "/snapshot.dat";
    snapshot_tmp_path_ = wal_dir + "/snapshot.dat.tmp";
  }

  LogEntry sentinel;
  sentinel.entry.set_term(0);
  sentinel.entry.set_command("COMMON_PREFIX");
  AddToLog(sentinel, false);
  last_log_index_ = 0;

  in_flight_vecs_.resize(total_num_ + 1);
  for (auto& vec : in_flight_vecs_) {
    vec.reserve(max_in_flight_per_follower_);
  }
  next_index_.assign(total_num_ + 1, last_log_index_ + 1);
  match_index_.assign(total_num_ + 1, last_log_index_);
  snapshot_in_progress_.assign(total_num_ + 1, false);
}

Raft::~Raft() { is_stop_ = true; }

bool Raft::IsStop() { return is_stop_; }

void Raft::SetRoleLocked(Role role) { role_ = role; }

void Raft::SetRole(Role role) {
  std::lock_guard<std::mutex> lk(mutex_);
  role_ = role;
}

bool Raft::ReceiveTransaction(std::unique_ptr<Request> req) {
  std::vector<AeFields> messages;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ != Role::LEADER) {
      LOG(INFO) << __FUNCTION__ << ": Replica is not leader, returning early";
      return false;
    }
    // Append new transaction to log.
    LogEntry log_entry;
    log_entry.entry.set_term(current_term_);

    std::string serialized;
    if (!req->SerializeToString(&serialized)) {
      LOG(INFO) << __FUNCTION__ << ": req could not be serialized";
      return false;
    }

    log_entry.entry.set_command(std::move(serialized));
    log_entry.GetSerializedSize();
    AddToLog(log_entry);

    next_index_[id_] = last_log_index_ + 1;
    match_index_[id_] = last_log_index_;

    if (replication_logging_flag_) {
      LOG(INFO) << __FUNCTION__ << ": Leader appended entry at index "
                << last_log_index_;
    }

    // Prepare fields for AppendEntries message.
    PruneExpiredInFlightMsgsLocked();
    messages = GatherAeFieldsForBroadcastLocked();
    auto now = std::chrono::steady_clock::now();
    for (const auto& msg : messages) {
      RecordNewInFlightMsgLocked(msg, now);
    }
  }
  for (const auto& msg : messages) {
    CreateAndSendAppendEntryMsg(msg);
  }
  leader_election_manager_->OnAeBroadcast();
  return true;
}

bool Raft::ReceiveAppendEntries(std::unique_ptr<AppendEntries> ae) {
  if (ae->leader_id() == id_) {
    return false;
  }
  uint64_t term;
  bool success = false;
  bool demoted = false;
  TermRelation tr;
  Role initial_role;
  uint64_t last_log_index;
  auto leader_commit = ae->leader_commit_index();
  auto leader_id = ae->leader_id();
  std::vector<std::unique_ptr<Request>> entries_to_apply;

  const char* parent_fn = __FUNCTION__;
  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;
    last_log_index = last_log_index_;
    tr = TermCheckLocked(ae->term());
    if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(ae->term());
    } else if (role_ != Role::FOLLOWER && tr == TermRelation::CURRENT) {
      demoted = DemoteSelfLocked(ae->term());
    }

    if (tr != TermRelation::STALE && role_ == Role::FOLLOWER) {
      uint64_t i = ae->prev_log_index();

      if (i <= snapshot_last_index_ ||
          (i < static_cast<uint64_t>(GetLogicalLogSize()) &&
           ae->prev_log_term() == GetLogTermAtIndex(i))) {
        success = true;
      }
    }
    term = current_term_;
    // Early return if we should not append.
    if (!success) {
      return;
    }

    // Append the entries to the log.
    uint64_t log_idx = ae->prev_log_index() + 1;
    uint64_t entries_idx = 0;
    // If we receive an entry that has already been committed, it must be
    // identical to what we have. So, skip to the first entry after the
    // committed entry.
    assert(snapshot_last_index_ <= commit_index_);
    if (log_idx <= commit_index_) {
      entries_idx = commit_index_ - log_idx + 1;
      log_idx = commit_index_ + 1;
    }
    uint64_t entries_size = static_cast<uint64_t>(ae->entries_size());
    // Check for conflicting entry terms in existing indices.
    // If there is a conflict, delete the suffix and short circuit out of the
    // loop.
    while (log_idx < GetLogicalLogSize() && entries_idx < entries_size) {
      uint64_t term = ae->entries(entries_idx).term();
      if (term != GetLogTermAtIndex(log_idx)) {
        TruncateLog(log_idx);

        if (replication_logging_flag_) {
          LOG(INFO) << parent_fn << ": follower saw term mismatch at index "
                    << log_idx << ". Suffix erased from log";
        }

        break;
      }
      ++entries_idx;
      ++log_idx;
    }

    // Append remaining entries.
    const auto append_size = entries_size - entries_idx;
    std::vector<LogEntry> log_entries_to_add;
    for (uint64_t i = entries_idx; i < entries_size; ++i) {
      log_entries_to_add.push_back(CreateLogEntry(ae->entries(i)));
    }

    uint64_t firstAppend_idx = last_log_index_ + 1;
    AddToLog(std::move(log_entries_to_add));
    last_log_index = last_log_index_;

    if (replication_logging_flag_ && append_size > 0) {
      if (append_size > 1) {
        LOG(INFO) << parent_fn << ": follower appended entries at indices "
                  << firstAppend_idx << " to " << last_log_index_;
      } else {
        LOG(INFO) << parent_fn << ": follower appended entry at index "
                  << last_log_index_;
      }
    }

    // Try to raise commit_index and commit entries
    uint64_t prev_commit_index = commit_index_;
    if (leader_commit > commit_index_) {
      commit_index_ = std::min(leader_commit, last_log_index_);

      if (replication_logging_flag_ && commit_index_ > prev_commit_index) {
        LOG(INFO) << parent_fn << ": Raised commit_index_ from "
                  << prev_commit_index << " to " << commit_index_;
      }
    }

    // Build the vector to apply committed entries outside mutex.
    entries_to_apply = PrepareCommitLocked();
  }();

  // Inform leader_election_manager, apply committed entries, and send response.
  if (demoted) {
    leader_election_manager_->OnRoleChange();
    LOG(INFO) << __FUNCTION__ << ": Demoted from "
              << (initial_role == Role::LEADER ? "LEADER" : "CANDIDATE")
              << "->FOLLOWER in term " << term;
  }

  if (tr != TermRelation::STALE) {
    leader_election_manager_->OnHeartBeat();
  }

  for (auto& entry : entries_to_apply) {
    commit_(*entry);
  }

  AppendEntriesResponse aer;
  aer.set_term(term);
  aer.set_success(success);
  aer.set_id(id_);
  aer.set_last_log_index(last_log_index);
  SendMessage(MessageType::AppendEntriesResponseMsg, aer, leader_id);

  return true;
}

bool Raft::ReceiveAppendEntriesResponse(
    std::unique_ptr<AppendEntriesResponse> aer) {
  uint64_t term;
  bool demoted = false;
  bool resending = false;
  TermRelation tr;
  Role initial_role;
  std::vector<std::unique_ptr<Request>> entries_to_apply;
  AeFields fields;
  int follower_id = aer->id();
  const char* parent_fn = __FUNCTION__;
  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;
    tr = TermCheckLocked(aer->term());
    if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(aer->term());
    }
    term = current_term_;

    if (role_ != Role::LEADER || tr == TermRelation::STALE) {
      return;
    }
    PruneExpiredInFlightMsgsLocked();
    PruneRedundantInFlightMsgsLocked(follower_id, aer->last_log_index());
    // next_index_ should never be greather than 1 + last_log_index_, or else
    // the leader may try to send an entry it does not have. However, it must be
    // greater than match_index_ for that follower, because we only update
    // match_index_ when we know an entry has been durably stored on that
    // follower.
    next_index_[follower_id] =
        std::max(std::min(aer->last_log_index() + 1, last_log_index_ + 1),
                 match_index_[follower_id] + 1);

    // If successful, update match_index and try to commit more entries.
    if (aer->success()) {
      // Need to ensure match_index never decreases even if any followers'
      // last_log_index decreases.
      match_index_[follower_id] =
          std::max(match_index_[follower_id], aer->last_log_index());
      // Use the updated match_index to find new entries eligible for commit.
      std::vector<uint64_t> sorted = match_index_;
      std::sort(sorted.begin(), sorted.end(), std::greater<uint64_t>());
      uint64_t last_replicated_index = sorted[quorum_ - 1];
      // Need to check the last_replicated_index contains entry from current
      // term.
      if (last_replicated_index > commit_index_ &&
          GetLogTermAtIndex(last_replicated_index) == current_term_) {
        LOG(INFO) << parent_fn << ": Raised commit_index_ from "
                  << commit_index_ << " to " << last_replicated_index;
        commit_index_ = last_replicated_index;
      }
      // Apply any newly committed entries to state machine.
      entries_to_apply = PrepareCommitLocked();
    }

    if (!aer->success() || (next_index_[follower_id] < last_log_index_ + 1)) {
      if (!aer->success()) {
        LOG(INFO) << "AppendEntriesResponse indicates FAILURE from follower "
                  << follower_id;
        LOG(INFO) << "next_index is: " << next_index_[follower_id]
                  << " their last_log_index is: " << aer->last_log_index();
      }
      if (aer->last_log_index() < snapshot_last_index_) {
        SendInstallSnapshot(follower_id, 0);
      } else if (!InFlightPerFollowerLimitReachedLocked(follower_id)) {
        fields = GatherAeFieldsLocked(follower_id);
        resending = true;
        auto now = std::chrono::steady_clock::now();
        RecordNewInFlightMsgLocked(fields, now);
      }
    }
  }();
  if (demoted) {
    leader_election_manager_->OnRoleChange();
    LOG(INFO) << __FUNCTION__ << ": Demoted from "
              << (initial_role == Role::LEADER ? "LEADER" : "CANDIDATE")
              << "->FOLLOWER in term " << term;
    return false;
  }
  if (resending) {
    CreateAndSendAppendEntryMsg(fields);
  }

  for (auto& entry : entries_to_apply) {
    commit_(*entry);
  }
  return true;
}

void Raft::ReceiveRequestVote(std::unique_ptr<RequestVote> rv) {
  int rv_sender = rv->candidateid();
  uint64_t rv_term = rv->term();

  uint64_t term;
  bool vote_granted = false;
  bool demoted = false;
  bool valid_candidate = false;
  int voted_for = -1;
  Role initial_role;

  if (rv_sender == id_) {
    return;
  }

  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;
    // If their term is higher than ours, we accept new term, reset voted_for
    // and convert to follower.
    TermRelation tr = TermCheckLocked(rv_term);
    if (tr == TermRelation::STALE) {
      term = current_term_;
      return;
    } else if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(rv_term);
    }
    // Then we continue voting process.
    term = current_term_;
    voted_for = voted_for_;

    uint64_t last_log_term = GetLastLogTermLocked();
    if (rv->lastlogterm() < last_log_term) {
      return;
    }
    if (rv->lastlogterm() == last_log_term &&
        rv->last_log_index() < last_log_index_) {
      return;
    }
    valid_candidate = true;
    if (voted_for_ == -1 || voted_for_ == rv_sender) {
      SetVotedFor(rv_sender);
      vote_granted = true;
    }
  }();
  if (demoted) {
    leader_election_manager_->OnRoleChange();
    LOG(INFO) << __FUNCTION__ << ": Demoted from "
              << (initial_role == Role::LEADER ? "LEADER" : "CANDIDATE")
              << "->FOLLOWER in term " << term;
  }
  if (vote_granted) {
    leader_election_manager_->OnHeartBeat();
    LOG(INFO) << __FUNCTION__ << ": voted for " << rv_sender << " in term "
              << term;
  } else if (valid_candidate) {
    LOG(INFO) << __FUNCTION__ << ": did not vote for " << rv_sender
              << " on term " << term << ". I already voted for " << voted_for
              << ((voted_for == id_) ? " (myself)" : "");
  }

  RequestVoteResponse rvr;
  rvr.set_term(term);
  rvr.set_voterid(id_);
  rvr.set_votegranted(vote_granted);
  SendMessage(MessageType::RequestVoteResponseMsg, rvr, rv_sender);
}

void Raft::ReceiveRequestVoteResponse(
    std::unique_ptr<RequestVoteResponse> rvr) {
  uint64_t term = rvr->term();
  int voter_id = rvr->voterid();
  bool voted_yes = rvr->votegranted();
  bool demoted = false;
  bool elected = false;
  Role initial_role;

  const char* parent_fn = __FUNCTION__;
  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;
    TermRelation tr = TermCheckLocked(term);
    if (tr == TermRelation::STALE) {
      return;
    } else if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(term);
      return;
    }
    if (role_ != Role::CANDIDATE) {
      return;
    }
    if (!voted_yes) {
      return;
    }
    bool dupe =
        (std::find(votes_.begin(), votes_.end(), voter_id) != votes_.end());
    if (dupe) {
      return;
    }
    votes_.push_back(voter_id);
    LOG(INFO) << parent_fn << ": Replica " << voter_id
              << " voted for me. Votes: " << votes_.size() << "/" << quorum_
              << " in term " << current_term_;
    if (votes_.size() >= quorum_) {
      elected = true;
      SetRoleLocked(Role::LEADER);
      ClearInFlightsLocked();
      next_index_.assign(total_num_ + 1, last_log_index_ + 1);

      // Make sure to set the leader's own match_index entry to last_log_index.
      match_index_.assign(total_num_ + 1, 0);
      match_index_[id_] = last_log_index_;
      LOG(INFO) << parent_fn << ": CANDIDATE->LEADER in term " << current_term_;
    }
  }();
  if (demoted || elected) {
    leader_election_manager_->OnRoleChange();
  }
  if (demoted) {
    LOG(INFO) << __FUNCTION__ << ": Demoted from "
              << (initial_role == Role::LEADER ? "LEADER" : "CANDIDATE")
              << "->FOLLOWER in term " << term;
  }
  if (elected) {
    SendHeartBeat();
  }
}

Role Raft::GetRoleSnapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return role_;
}

// Called from LeaderElectionManager::StartElection when timeout.
void Raft::StartElection() {
  uint64_t current_term;
  int candidate_id;
  uint64_t last_log_index;
  uint64_t last_log_term;
  bool role_changed = false;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ == Role::LEADER) {
      LOG(WARNING) << __FUNCTION__ << ": Leader tried to start election";
      return;
    }
    if (role_ == Role::FOLLOWER) {
      SetRoleLocked(Role::CANDIDATE);
      role_changed = true;
    }
    heartbeats_sent_this_term_ = 0;
    SetCurrentTermAndVotedFor(current_term_ + 1, id_);
    votes_.clear();
    votes_.push_back(id_);
    LOG(INFO) << __FUNCTION__
              << ": I voted for myself. Votes: " << votes_.size() << "/"
              << quorum_ << " in term " << current_term_;

    current_term = current_term_;
    candidate_id = id_;
    last_log_index = last_log_index_;
    last_log_term = GetLastLogTermLocked();
  }
  if (role_changed) {
    leader_election_manager_->OnRoleChange();
    LOG(INFO) << __FUNCTION__ << ": FOLLOWER->CANDIDATE in term "
              << current_term;
  }

  RequestVote rv;
  rv.set_term(current_term);
  rv.set_candidateid(candidate_id);
  rv.set_last_log_index(last_log_index);
  rv.set_lastlogterm(last_log_term);
  Broadcast(MessageType::RequestVoteMsg, rv);
}

void Raft::SendHeartBeat() {
  auto function_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration function_delta;

  std::vector<AeFields> messages;
  uint64_t current_term;
  uint64_t heartBeat_num;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ != Role::LEADER) {
      LOG(WARNING) << __FUNCTION__ << ": Non-Leader tried to start HeartBeat";
      return;
    }
    current_term = current_term_;

    heartbeats_sent_this_term_++;
    heartBeat_num = heartbeats_sent_this_term_;
    bool heartbeat = true;
    messages = GatherAeFieldsForBroadcastLocked(heartbeat);
  }

  auto msg_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration msg_delta;

  for (const auto& msg : messages) {
    CreateAndSendAppendEntryMsg(msg);
  }

  auto msg_end = std::chrono::steady_clock::now();
  msg_delta = msg_end - msg_start;
  auto msg_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(msg_delta).count();

  if (liveness_logging_flag_) {
    LOG(INFO) << __FUNCTION__ << ": " << msg_ms
              << " ms elapsed in CreateAndSend loop";
    LOG(INFO) << __FUNCTION__ << ": Heartbeat " << heartBeat_num << " for term "
              << current_term;
  }

  auto redirect_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration redirect_delta;

  // Ping client proxies that this is the leader.
  DirectToLeader dtl;
  dtl.set_term(current_term);
  dtl.set_leader_id(id_);
  for (const auto& client : replica_communicator_->GetClientReplicas()) {
    int id = client.id();
    SendMessage(DirectToLeaderMsg, dtl, id);
  }

  auto redirect_end = std::chrono::steady_clock::now();
  redirect_delta = redirect_end - redirect_start;
  auto redirect_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(redirect_delta)
          .count();

  auto function_end = std::chrono::steady_clock::now();
  function_delta = function_end - function_start;
  auto function_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(function_delta)
          .count();

  if (liveness_logging_flag_) {
    LOG(INFO) << __FUNCTION__ << ": " << redirect_ms
              << " ms elapsed in redirect loop";
    LOG(INFO) << __FUNCTION__ << ": " << function_ms
              << " ms elapsed in function";
  }
}

// Requires the raft mutex to be held.
// Returns true if demoted.
bool Raft::DemoteSelfLocked(uint64_t term) {
  if (term > current_term_) {
    SetCurrentTermAndVotedFor(term, -1);
  }
  if (role_ != Role::FOLLOWER) {
    SetRoleLocked(Role::FOLLOWER);
    return true;
  }
  return false;
}

// Requires the raft mutex to be held.
TermRelation Raft::TermCheckLocked(uint64_t term) const {
  if (term < current_term_) {
    return TermRelation::STALE;
  } else if (term == current_term_) {
    return TermRelation::CURRENT;
  } else {
    return TermRelation::NEW;
  }
}

// Requires the raft mutex to be held.
uint64_t Raft::GetLastLogTermLocked() const {
  if (last_log_index_ <= snapshot_last_index_) {
    return snapshot_last_term_;
  }

  return GetLogTermAtIndex(last_log_index_);
}

// Requires the raft mutex to be held.
std::vector<std::unique_ptr<Request>> Raft::PrepareCommitLocked() {
  std::vector<std::unique_ptr<Request>> commit_vec;
  uint64_t begin = last_committed_ + 1;
  bool applying = false;
  while (last_committed_ < commit_index_ &&
         last_committed_ < GetLogicalLogSize() - 1) {
    ++last_committed_;
    auto command = std::make_unique<Request>();

    if (!command->ParseFromString(
            GetLogEntryAtIndex(last_committed_).entry.command())) {
      LOG(INFO) << __FUNCTION__ << ": Failed to parse command";
      continue;
    }
    // seq must be the log index for the request or executing transactions
    // fails.
    command->set_seq(last_committed_);
    commit_vec.push_back(std::move(command));
    applying = true;
  }

  if (applying && replication_logging_flag_) {
    if (last_committed_ > begin) {
      LOG(INFO) << __FUNCTION__ << ": Applying index entries " << begin
                << " to " << last_committed_;
    } else {
      LOG(INFO) << __FUNCTION__ << ": Applying index entry " << last_committed_;
    }
  }

  return commit_vec;
}

// Requires the raft mutex to be held.
AeFields Raft::GatherAeFieldsLocked(int follower_id, bool heart_beat) const {
  AeFields fields{};
  LOG(INFO) << "snapshot_last_index_ is: " << snapshot_last_index_;
  assert((next_index_[follower_id] - 1 >= snapshot_last_index_) || heart_beat);

  fields.term = current_term_;
  fields.leader_id = id_;
  fields.leader_commit = commit_index_;
  fields.prev_log_index = next_index_[follower_id] - 1;
  fields.prev_log_term = GetLogTermAtIndex(fields.prev_log_index);
  fields.follower_id = follower_id;
  if (heart_beat) {
    return fields;
  }
  uint32_t msg_bytes = max_header_bytes_;
  const uint64_t first_new = next_index_[follower_id];
  const uint64_t limit =
      std::min(last_log_index_, (first_new + max_entries_) - 1);
  for (uint64_t i = first_new; i <= limit; ++i) {
    msg_bytes += GetLogEntryAtIndex(i).GetSerializedSize();
    // Always include at least 1 entry, after that limit by max_bytes_.
    if (i != first_new && msg_bytes >= max_bytes_) {
      break;
    }
    LogEntry entry;
    entry.entry = GetLogEntryAtIndex(i).entry;
    fields.entries.push_back(entry);
  }
  return fields;
}

// Returns vector of tuples <follower_id, AeFields>.
// If heart_beat == true, entries[] will be empty for all messages,
// else entries will each contain at most max_entries_ amount of entries.
// Followers will be excluded from the broadcast if they are at inflight max
// unless this is a heartbeat.
std::vector<AeFields> Raft::GatherAeFieldsForBroadcastLocked(
    bool heart_beat) const {
  assert(role_ == Role::LEADER);
  std::vector<AeFields> fields_vec;
  fields_vec.reserve(total_num_ - 1);
  for (size_t i = 1; i <= total_num_; ++i) {
    if (i == id_) {
      continue;
    }
    if (!heart_beat && InFlightPerFollowerLimitReachedLocked(i)) {
      continue;
    }
    if (next_index_[i] - 1 >= snapshot_last_index_) {
      AeFields fields = GatherAeFieldsLocked(i, heart_beat);
      fields_vec.push_back(fields);
    }
  }
  return fields_vec;
}

void Raft::CreateAndSendAppendEntryMsg(const AeFields& fields) {
  int follower_id = fields.follower_id;
  AppendEntries ae;
  ae.set_term(fields.term);
  ae.set_leader_id(fields.leader_id);
  ae.set_prev_log_index(fields.prev_log_index);
  ae.set_prev_log_term(fields.prev_log_term);
  ae.set_leader_commit_index(fields.leader_commit);
  for (const auto& entry : fields.entries) {
    Entry* new_entry = ae.add_entries();
    new_entry->set_term(entry.entry.term());
    new_entry->set_command(entry.entry.command());
  }
  SendMessage(MessageType::AppendEntriesMsg, ae, follower_id);
  if (replication_logging_flag_) {
    uint64_t entry_count = fields.entries.size();
    LOG(INFO) << __FUNCTION__ << ": Sent AE with " << entry_count
              << (entry_count == 1 ? " entry" : " entries");
  }
}

LogEntry Raft::CreateLogEntry(const Entry& entry) const {
  LogEntry new_entry;
  new_entry.entry = entry;
  return new_entry;
}

// Requires the raft mutex to be held.
void Raft::ClearInFlightsLocked() {
  assert(role_ == Role::LEADER);
  for (auto& vec : in_flight_vecs_) {
    vec.clear();
  }
}

// Requires the raft mutex to be held.
void Raft::PruneExpiredInFlightMsgsLocked() {
  assert(role_ == Role::LEADER);
  auto now = std::chrono::steady_clock::now();
  for (size_t i = 1; i < in_flight_vecs_.size(); ++i) {
    if (i == id_) {
      continue;
    }
    auto& vec = in_flight_vecs_[i];
    if (vec.empty()) {
      continue;
    }
    auto it = vec.begin();
    while (it != vec.end()) {
      auto time_elapsed = now - it->time_sent;
      if (time_elapsed >= ae_response_deadline_) {
        it = vec.erase(it);
        if (replication_logging_flag_) {
          LOG(INFO) << __FUNCTION__
                    << ": Pruned expired inflight AE for follower " << i;
        }
      } else {
        ++it;
      }
    }
  }
}

void Raft::PruneRedundantInFlightMsgsLocked(int follower_id,
                                            uint64_t followerlast_log_index) {
  assert(role_ == Role::LEADER);
  assert(follower_id > 0);
  assert(static_cast<size_t>(follower_id) < in_flight_vecs_.size());
  assert(follower_id != id_);

  auto& msg_vec = in_flight_vecs_[follower_id];
  if (msg_vec.empty()) {
    return;
  }
  auto it = msg_vec.begin();
  while (it != msg_vec.end()) {
    if (it->prev_log_index_sent > followerlast_log_index ||
        it->last_index_of_segment_sent <= followerlast_log_index) {
      it = msg_vec.erase(it);
      if (replication_logging_flag_) {
        LOG(INFO) << __FUNCTION__
                  << ": Pruned redundant inflight AE for follower "
                  << follower_id;
      }
    } else {
      ++it;
    }
  }
}

void Raft::RecordNewInFlightMsgLocked(
    const AeFields& msg, std::chrono::steady_clock::time_point timestamp) {
  if (msg.entries.empty()) {
    return;
  }
  InFlightMsg in_flight;
  in_flight.time_sent = timestamp;
  in_flight.prev_log_index_sent = msg.prev_log_index;
  in_flight.last_index_of_segment_sent =
      msg.prev_log_index + msg.entries.size();
  in_flight_vecs_[msg.follower_id].push_back(in_flight);
}

// Requires the raft mutex to be held.
bool Raft::InFlightPerFollowerLimitReachedLocked(int follower_id) const {
  assert(role_ == Role::LEADER);
  assert(follower_id > 0);
  assert(static_cast<size_t>(follower_id) < in_flight_vecs_.size());
  assert(follower_id != id_);

  auto size = in_flight_vecs_[follower_id].size();
  assert(size <= max_in_flight_per_follower_);
  return size == max_in_flight_per_follower_;
}

const LogEntry& Raft::GetLogEntryAtIndex(uint64_t index) const {
  assert(index > snapshot_last_index_ &&
         "Tried to access entry that has been snapshotted");
  // A sentinel value is always included after a snapshot
  // Example: snapshot_last_index_ = 5, we have truncated the entire log, added
  // 1 entry, then log_.size() == 2 with the sentinel. index could be 6, and
  // snapshot_last_index_ + log_.size() == 7.
  assert(index < snapshot_last_index_ + log_.size() &&
         "Tried to access element that has not been added yet");
  return log_[index - snapshot_last_index_];
}

const uint64_t Raft::GetLogTermAtIndex(uint64_t index) const {
  assert(index >= snapshot_last_index_ &&
         "Tried to access entry that has been snapshotted");
  assert(index < snapshot_last_index_ + log_.size() &&
         "Tried to access element that has not been added yet");
  if (index == snapshot_last_index_) {
    return snapshot_last_term_;
  }

  return log_[index - snapshot_last_index_].entry.term();
}

// This would be what log_.size() returns if no prefix truncation occurred.
int Raft::GetLogicalLogSize() const {
  return log_.size() + snapshot_last_index_;
}

void Raft::SetCurrentTerm(uint64_t current_term, bool write_metadata) {
  current_term_ = current_term;
  if (write_metadata) {
    WriteMetadata();
  }
}

void Raft::SetVotedFor(int voted_for, bool write_metadata) {
  voted_for_ = voted_for;
  if (write_metadata) {
    WriteMetadata();
  }
}

void Raft::SetCurrentTermAndVotedFor(uint64_t current_term, int voted_for,
                                     bool write_metadata) {
  current_term_ = current_term;
  voted_for_ = voted_for;
  if (write_metadata) {
    WriteMetadata();
  }
}

void Raft::SetSnapshotLastIndexAndTerm(uint64_t snapshot_last_index,
                                       uint64_t snapshot_last_term,
                                       bool write_metadata) {
  snapshot_last_index_ = snapshot_last_index;
  snapshot_last_term_ = snapshot_last_term;
  log_[0].entry.set_term(snapshot_last_term_);
  LOG(INFO) << "setting snapshot_last_index " << snapshot_last_index
            << " and snapshot_last_term" << snapshot_last_term;
  if (write_metadata) {
    WriteMetadata();
    return;
  }

  // Function is only called with write_metadata == false on initial recovery,
  // so these variables need to be set.
  last_log_index_ = snapshot_last_index_;
  commit_index_ = snapshot_last_index_;
  last_committed_ = snapshot_last_index_;
}

uint64_t Raft::GetSnapshotLastIndex() { return snapshot_last_index_; }

void Raft::WriteMetadata() {
  recovery_->WriteMetadata(current_term_, voted_for_, snapshot_last_index_,
                           snapshot_last_term_);
}

void Raft::AddToLog(LogEntry& log_entry_to_add, bool write_metadata) {
  last_log_index_++;
  Entry* entry;
  entry = &log_entry_to_add.entry;
  if (write_metadata) {
    recovery_->AddLogEntry(entry, last_log_index_);
  }
  log_.push_back(log_entry_to_add);
  assert(last_log_index_ == GetLogicalLogSize() - 1);
}

void Raft::AddToLog(std::vector<LogEntry> log_entries_to_add,
                    bool write_metadata) {
  if (write_metadata) {
    std::vector<Entry> entries_to_add;
    for (const auto& entry : log_entries_to_add) {
      entries_to_add.push_back(entry.entry);
    }
    recovery_->AddLogEntry(entries_to_add, last_log_index_ + 1);
  }

  last_log_index_ += log_entries_to_add.size();
  log_.reserve(log_.size() + log_entries_to_add.size());
  log_.insert(log_.end(), std::make_move_iterator(log_entries_to_add.begin()),
              std::make_move_iterator(log_entries_to_add.end()));

  assert(last_log_index_ == GetLogicalLogSize() - 1);
}

void Raft::TruncateLog(uint64_t first_index, bool write_metadata) {
  assert(first_index > commit_index_);
  auto first = log_.begin() + (first_index - snapshot_last_index_);
  auto last = log_.begin() + (last_log_index_ - snapshot_last_index_) + 1;
  auto num_elements_erased = last_log_index_ - first_index + 1;
  if (write_metadata) {
    TruncationRecord truncation;
    truncation.set_truncate_from_index(first_index);
    truncation.set_truncate_from_term(GetLogTermAtIndex(first_index));
    recovery_->TruncateLog(truncation);
  }

  log_.erase(first, last);
  last_log_index_ -= num_elements_erased;
  assert(last_log_index_ == GetLogicalLogSize() - 1);
}

void Raft::TruncatePrefix(uint64_t index) {
  std::lock_guard<std::mutex> lk(mutex_);
  TruncatePrefixLocked(index);
}

// Requires the raft mutex to be held.
void Raft::TruncatePrefixLocked(uint64_t index) {
  assert(index > snapshot_last_index_ &&
         "Tried to truncate an entry that has been snapshotted");
  assert(index <= last_committed_ &&
         "Tried to prefix truncate an element that has not been committed");
  LOG(INFO) << "Setting Snapshot last index to:" << index;

  // Keep the sentinel, erase everything up to the index.
  auto erase_end = log_.begin() + (index - snapshot_last_index_);
  auto last_snapshotted_entry_term = GetLogTermAtIndex(index);
  log_.erase(log_.begin() + 1, erase_end + 1);
  SetSnapshotLastIndexAndTerm(index, last_snapshotted_entry_term);

  assert(last_log_index_ == GetLogicalLogSize() - 1);
}

// Serialize the storage state machine snapshot.
// Format per entry: [4-byte key_len][key][4-byte val_len][val].
static std::string SerializeSnapshot(Storage* storage) {
  LOG(INFO) << "Serializing Storage snapshot";
  std::string data;
  if (!storage) {
    return data;
  }

  for (const auto& [key, value_ver] : storage->GetAllItems()) {
    const std::string& value = value_ver.first;
    uint32_t key_length = static_cast<uint32_t>(key.size());
    uint32_t value_length = static_cast<uint32_t>(value.size());
    data.append(reinterpret_cast<const char*>(&key_length), 4);
    data.append(key);
    data.append(reinterpret_cast<const char*>(&value_length), 4);
    data.append(value);
  }
  return data;
}

static void ApplySnapshot(Storage* storage, const std::string& raw) {
  if (!storage) {
    return;
  }
  LOG(INFO) << "Applying Snapshot";

  // Clear existing storage before applying snapshot.
  storage->Clear();
  if (raw.empty()) {
    storage->Flush(true);
    return;
  }
  size_t pos = 0;
  while (pos + 8 <= raw.size()) {
    uint32_t key_length;
    std::memcpy(&key_length, raw.data() + pos, 4);
    pos += 4;
    if (pos + key_length > raw.size()) {
      break;
    }
    std::string key(raw.data() + pos, key_length);
    pos += key_length;

    uint32_t value_length;
    if (pos + 4 > raw.size()) {
      break;
    }
    std::memcpy(&value_length, raw.data() + pos, 4);
    pos += 4;
    if (pos + value_length > raw.size()) {
      break;
    }
    std::string val(raw.data() + pos, value_length);
    pos += value_length;

    storage->SetValue(key, val);
  }
  // Flush storage to disk before WriteMetadata().
  storage->Flush(/*should_sync=*/true);
}

// Send one chunk of a snapshot to a follower whose log has fallen behind the
// entries remaining in the leader's log. Before the first chunk is sent, the
// snapshot is written to disk. Then it is read back one chunk at a time.
// Subsequent chunks are sent after the follower ACKs each one.
void Raft::SendInstallSnapshot(int follower_id, size_t byte_offset) {
  LOG(INFO) << "SendInstallSnapshot to follower " << follower_id
            << " at offset " << byte_offset;

  // Gather snapshot metadata under the lock.
  uint64_t term;
  uint64_t last_included_index;
  uint64_t last_included_term;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    term = current_term_;
    last_included_index = snapshot_last_index_;
    last_included_term = snapshot_last_term_;
  }

  // For the first chunk, (re-)serialize the state machine to disk so the file
  // is consistent with the current snapshot point. Write to a temp path then
  // rename so we never serve a partially-written file.
  if (byte_offset == 0) {
    std::string serialized = SerializeSnapshot(recovery_->GetStorage());

    int tmp_fd =
        open(snapshot_tmp_path_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (tmp_fd < 0) {
      LOG(ERROR) << "SendInstallSnapshot: failed to open tmp snapshot file "
                 << snapshot_tmp_path_ << ": " << strerror(errno);
      return;
    }

    const char* ptr = serialized.data();
    size_t remaining = serialized.size();
    while (remaining > 0) {
      ssize_t written = write(tmp_fd, ptr, remaining);
      if (written <= 0) {
        LOG(ERROR) << "SendInstallSnapshot: write failed: " << strerror(errno);
        close(tmp_fd);
        unlink(snapshot_tmp_path_.c_str());
        return;
      }
      ptr += written;
      remaining -= static_cast<size_t>(written);
    }

    if (fsync(tmp_fd) < 0) {
      LOG(ERROR) << "SendInstallSnapshot: fsync failed: " << strerror(errno);
      close(tmp_fd);
      unlink(snapshot_tmp_path_.c_str());
      return;
    }
    close(tmp_fd);

    if (rename(snapshot_tmp_path_.c_str(), snapshot_file_path_.c_str()) < 0) {
      LOG(ERROR) << "SendInstallSnapshot: rename failed: " << strerror(errno);
      unlink(snapshot_tmp_path_.c_str());
      return;
    }
  }

  // Open the committed snapshot file and read one chunk starting at
  // byte_offset.
  int snap_fd = open(snapshot_file_path_.c_str(), O_RDONLY);
  if (snap_fd < 0) {
    LOG(ERROR) << "SendInstallSnapshot: failed to open snapshot file "
               << snapshot_file_path_ << ": " << strerror(errno);
    return;
  }

  struct stat st;
  if (fstat(snap_fd, &st) < 0) {
    LOG(ERROR) << "SendInstallSnapshot: fstat failed: " << strerror(errno);
    close(snap_fd);
    return;
  }
  size_t total_size = static_cast<size_t>(st.st_size);

  if (byte_offset > total_size) {
    LOG(WARNING) << "SendInstallSnapshot: byte_offset " << byte_offset
                 << " exceeds snapshot size " << total_size;
    close(snap_fd);
    return;
  }

  if (lseek(snap_fd, static_cast<off_t>(byte_offset), SEEK_SET) < 0) {
    LOG(ERROR) << "SendInstallSnapshot: lseek failed: " << strerror(errno);
    close(snap_fd);
    return;
  }

  size_t chunk_size = std::min(chunk_size_in_bytes_, total_size - byte_offset);
  std::string chunk(chunk_size, '\0');
  size_t bytes_read = 0;
  while (bytes_read < chunk_size) {
    ssize_t n =
        read(snap_fd, chunk.data() + bytes_read, chunk_size - bytes_read);
    if (n <= 0) {
      LOG(ERROR) << "SendInstallSnapshot: read failed: " << strerror(errno);
      close(snap_fd);
      return;
    }
    bytes_read += static_cast<size_t>(n);
  }
  close(snap_fd);

  bool done = (byte_offset + chunk_size >= total_size);

  InstallSnapshot msg;
  msg.set_term(term);
  msg.set_leader_id(id_);
  msg.set_last_included_index(last_included_index);
  msg.set_last_included_term(last_included_term);
  msg.set_offset(byte_offset);
  msg.set_data(std::move(chunk));
  msg.set_done(done);

  LOG(INFO) << "SendInstallSnapshot to follower " << follower_id
            << " last_included_index=" << last_included_index
            << " offset=" << byte_offset << " total_bytes=" << total_size
            << " chunk_bytes=" << chunk_size << " done=" << done;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    snapshot_in_progress_[follower_id] = true;
  }
  SendMessage(MessageType::InstallSnapshotMsg, msg, follower_id);
}

bool Raft::ReceiveInstallSnapshot(std::unique_ptr<InstallSnapshot> is) {
  LOG(INFO) << "Snapshot chunk received";
  int leader_id = is->leader_id();
  uint64_t incoming_offset = is->offset();
  uint64_t last_included_index = is->last_included_index();
  uint64_t last_included_term = is->last_included_term();
  bool done = is->done();

  // Variables set inside the lock, acted on outside.
  bool demoted = false;
  bool should_install = false;
  uint64_t our_term = 0;
  uint64_t bytes_stored = 0;
  // Temp file path to rename into place after all chunks arrive (set outside
  // lock).
  std::string tmp_path_to_rename;

  // Helper to close and clean up a PendingSnapshot's temp file, then erase it.
  auto AbortPending = [&](std::map<int, PendingSnapshot>::iterator it) {
    LOG(INFO) << "New snapshot received while one was in progress, aborting.";
    if (it->second.fd >= 0) {
      close(it->second.fd);
    }
    if (!it->second.tmp_path.empty()) {
      unlink(it->second.tmp_path.c_str());
    }
    pending_snapshot_chunks_.erase(it);
  };

  {
    std::lock_guard<std::mutex> lk(mutex_);
    our_term = current_term_;

    TermRelation rel = TermCheckLocked(is->term());
    if (rel == TermRelation::STALE) {
      LOG(INFO) << "ReceiveInstallSnapshot: stale term " << is->term()
                << " (ours=" << our_term << ")";
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(false);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      return false;
    }
    leader_election_manager_->OnHeartBeat();
    if (rel == TermRelation::NEW) {
      demoted = DemoteSelfLocked(is->term());
      our_term = current_term_;
    }

    // If offset == 0 and the snapshot is older than what we already have, our
    // state is already further ahead.
    if (incoming_offset == 0) {
      assert(commit_index_ >= snapshot_last_index_);
      // If the snapshot contains the prefix to our log (either directly or
      // through snapshot), then reply rejecting the snapshot.
      if (last_included_index <= commit_index_ ||
          (last_included_index <= last_log_index_ &&
           GetLogTermAtIndex(last_included_index) == last_included_term)) {
        LOG(INFO) << "ReceiveInstallSnapshot: ignoring snapshot at index "
                  << last_included_index
                  << ", already have matching entry at index "
                  << last_included_index << " with term " << last_included_term;
        InstallSnapshotResponse isr;
        isr.set_term(our_term);
        isr.set_id(id_);
        isr.set_need_snapshot(false);
        isr.set_bytes_stored(0);
        isr.set_last_included_index(last_included_index);
        isr.set_transfer_complete(false);
        SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
        if (demoted) {
          leader_election_manager_->OnRoleChange();
        }
        return true;
      }

      // Discard any in-progress transfer from this leader and start fresh.
      auto existing = pending_snapshot_chunks_.find(leader_id);
      if (existing != pending_snapshot_chunks_.end()) {
        AbortPending(existing);
      }

      // Open a new temp file for this snapshot transfer.
      std::string tmp_path = snapshot_file_path_ + ".recv.tmp";
      int fd = open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
      if (fd < 0) {
        LOG(ERROR) << "ReceiveInstallSnapshot: failed to open recv tmp file "
                   << tmp_path << ": " << strerror(errno);
        InstallSnapshotResponse isr;
        isr.set_term(our_term);
        isr.set_id(id_);
        isr.set_need_snapshot(true);
        isr.set_bytes_stored(0);
        isr.set_last_included_index(last_included_index);
        isr.set_transfer_complete(false);
        SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
        if (demoted) {
          leader_election_manager_->OnRoleChange();
        }
        return false;
      }

      PendingSnapshot pending_snapshot;
      pending_snapshot.last_included_index = last_included_index;
      pending_snapshot.last_included_term = last_included_term;
      pending_snapshot.expected_offset = 0;
      pending_snapshot.fd = fd;
      pending_snapshot.tmp_path = std::move(tmp_path);
      pending_snapshot_chunks_[leader_id] = std::move(pending_snapshot);
    }

    auto it = pending_snapshot_chunks_.find(leader_id);
    // Non-first chunk with no transfer in progress.
    if (it == pending_snapshot_chunks_.end()) {
      LOG(WARNING) << "ReceiveInstallSnapshot: chunk at offset "
                   << incoming_offset << " but no pending transfer from leader "
                   << leader_id << "; requesting restart";
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }

    PendingSnapshot& pending = it->second;

    // Restart if this chunk belongs to a different snapshot.
    if (pending.last_included_index != last_included_index ||
        pending.last_included_term != last_included_term) {
      LOG(WARNING) << "ReceiveInstallSnapshot: chunk belongs to different "
                      "snapshot (index "
                   << last_included_index << " vs pending "
                   << pending.last_included_index << "); requesting restart";
      AbortPending(it);
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }

    // Reject out-of-order chunks and tell the leader what offset we expect
    // next.
    if (incoming_offset != pending.expected_offset) {
      LOG(WARNING) << "ReceiveInstallSnapshot: out-of-order chunk: expected "
                   << pending.expected_offset << " got " << incoming_offset;
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(pending.expected_offset);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }

    // Write chunk data to the temp file at the correct offset.
    {
      LOG(INFO) << "Writing snapshot chunk to file";
      const std::string& chunk = is->data();
      const char* ptr = chunk.data();
      size_t remaining = chunk.size();
      while (remaining > 0) {
        ssize_t written = write(pending.fd, ptr, remaining);
        LOG(INFO) << "writing snapshot chunk to: " << pending.tmp_path;
        if (written <= 0) {
          LOG(ERROR) << "ReceiveInstallSnapshot: write to temp file failed: "
                     << strerror(errno);
          AbortPending(it);
          InstallSnapshotResponse isr;
          isr.set_term(our_term);
          isr.set_id(id_);
          isr.set_need_snapshot(true);
          isr.set_bytes_stored(0);
          isr.set_last_included_index(last_included_index);
          isr.set_transfer_complete(false);
          SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
          if (demoted) {
            leader_election_manager_->OnRoleChange();
          }
          return false;
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
      }
      if (fsync(pending.fd) < 0) {
        LOG(ERROR) << "ReceiveInstallSnapshot: Failed to fsync to temp file"
                   << strerror(errno);
      }
      pending.expected_offset += static_cast<uint64_t>(chunk.size());
      bytes_stored = pending.expected_offset;
    }

    // Reply and wait for more chunks if not done.
    if (!done) {
      LOG(INFO) << "Snapshot chunk added, waiting for more";
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(bytes_stored);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return true;
    }

    // At this point, all chunks of the snapshot have been received.

    // Step 5: fsync and close the temp file, then set up for rename outside
    // lock.
    if (fsync(pending.fd) < 0) {
      LOG(ERROR) << "ReceiveInstallSnapshot: fsync failed: " << strerror(errno);
      AbortPending(it);
      InstallSnapshotResponse isr;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }
    LOG(INFO) << "Completed snapshot is fsynced to disk";
    close(pending.fd);
    pending.fd = -1;
    tmp_path_to_rename = pending.tmp_path;
    pending_snapshot_chunks_.erase(it);

    // Discard the entire log and update the sentinel.
    log_.clear();
    LogEntry sentinel;
    sentinel.entry.set_term(last_included_term);
    sentinel.entry.set_command("COMMON_PREFIX");
    log_.push_back(sentinel);

    snapshot_last_index_ = last_included_index;
    snapshot_last_term_ = last_included_term;
    last_log_index_ = last_included_index;
    commit_index_ = last_included_index;
    last_committed_ = last_included_index;

    should_install = true;
  }

  if (demoted) {
    leader_election_manager_->OnRoleChange();
  }

  if (should_install) {
    LOG(INFO) << "All snapshot chunks received, applying snapshot";
    // Atomically rename the temp file to the committed snapshot
    // path so we always have a consistent snapshot file on disk.
    if (rename(tmp_path_to_rename.c_str(), snapshot_file_path_.c_str()) < 0) {
      LOG(ERROR) << "ReceiveInstallSnapshot: rename failed: "
                 << strerror(errno);
    }

    // Read the snapshot back from the committed file and apply it.
    std::string full_data;
    {
      int snap_fd = open(snapshot_file_path_.c_str(), O_RDONLY);
      if (snap_fd < 0) {
        // Fall back to temp path if rename failed.
        snap_fd = open(tmp_path_to_rename.c_str(), O_RDONLY);
      }
      if (snap_fd < 0) {
        LOG(ERROR)
            << "ReceiveInstallSnapshot: failed to open snapshot for apply: "
            << strerror(errno);
      } else {
        struct stat st;
        if (fstat(snap_fd, &st) == 0) {
          full_data.resize(static_cast<size_t>(st.st_size));
          size_t bytes_read = 0;
          while (bytes_read < full_data.size()) {
            ssize_t n = read(snap_fd, full_data.data() + bytes_read,
                             full_data.size() - bytes_read);
            if (n <= 0) {
              LOG(ERROR) << "ReceiveInstallSnapshot: read failed: "
                         << strerror(errno);
              break;
            }
            bytes_read += static_cast<size_t>(n);
          }
        }
        close(snap_fd);
      }
    }

    ApplySnapshot(recovery_->GetStorage(), full_data);
    WriteMetadata();
    LOG(INFO) << "ReceiveInstallSnapshot: installed snapshot up to index="
              << last_included_index;
  }

  // Send final ACK.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    InstallSnapshotResponse isr;
    isr.set_term(current_term_);
    isr.set_id(id_);
    isr.set_need_snapshot(false);
    isr.set_bytes_stored(bytes_stored);
    isr.set_last_included_index(last_included_index);
    isr.set_transfer_complete(true);
    SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
  }
  return true;
}

bool Raft::ReceiveInstallSnapshotResponse(
    std::unique_ptr<InstallSnapshotResponse> isr) {
  int follower_id = isr->id();
  uint64_t last_included_index = isr->last_included_index();
  bool need_snapshot = isr->need_snapshot();
  uint64_t bytes_stored = isr->bytes_stored();
  bool transfer_complete = isr->transfer_complete();

  bool demoted = false;
  Role initial_role = Role::FOLLOWER;
  AeFields catchup_fields;
  bool send_catchup_ae = false;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;

    TermRelation rel = TermCheckLocked(isr->term());
    if (rel == TermRelation::NEW) {
      demoted = DemoteSelfLocked(isr->term());
    }

    if (role_ != Role::LEADER || rel == TermRelation::STALE) {
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }

    if (!need_snapshot) {
      snapshot_in_progress_[follower_id] = false;
      if (transfer_complete) {
        LOG(INFO) << "ReceiveInstallSnapshotResponse: snapshot complete for "
                  << "follower " << follower_id
                  << " last_included_index=" << last_included_index;
        next_index_[follower_id] = last_included_index + 1;
        match_index_[follower_id] =
            std::max(match_index_[follower_id], last_included_index);
        // If the follower still needs log entries, send them now.
        if (next_index_[follower_id] <= last_log_index_) {
          catchup_fields = GatherAeFieldsLocked(follower_id);
          send_catchup_ae = true;
        }
      } else {
        // The follower rejected the snapshot and does not need it.
        LOG(INFO) << "ReceiveInstallSnapshotResponse: Rejection from follower "
                  << follower_id;
      }
    }
  }

  if (demoted) {
    leader_election_manager_->OnRoleChange();
    LOG(INFO) << "ReceiveInstallSnapshotResponse: demoted from "
              << (initial_role == Role::LEADER ? "LEADER" : "CANDIDATE")
              << " to FOLLOWER";
    return false;
  }

  if (need_snapshot) {
    // Restart / continue the snapshot transfer.
    SendInstallSnapshot(follower_id, bytes_stored);
  } else if (send_catchup_ae) {
    CreateAndSendAppendEntryMsg(catchup_fields);
  }

  return true;
}

void Raft::PrintDebugState() const {
  std::lock_guard<std::mutex> lk(mutex_);
  PrintDebugStateLocked();
}

// Requires the raft mutex to be held.
void Raft::PrintDebugStateLocked() const {
  std::ostringstream oss;

  oss << "---- Raft Debug State ----\n";

  oss << "current_term_: " << current_term_ << "\n";
  oss << "voted_for_: " << voted_for_ << "\n";

  oss << "log_ (size " << GetLogicalLogSize() << "): [";
  for (size_t i = 0; i < GetLogicalLogSize(); ++i) {
    oss << "{term: " << GetLogTermAtIndex(i) << "}";
    if (i + 1 != GetLogicalLogSize()) {
      oss << ", ";
    }
  }
  oss << "]\n";

  oss << "next_index_: [";
  for (size_t i = 0; i < next_index_.size(); ++i) {
    oss << next_index_[i];
    if (i + 1 != next_index_.size()) {
      oss << ", ";
    }
  }
  oss << "]\n";

  oss << "match_index_: [";
  for (size_t i = 0; i < match_index_.size(); ++i) {
    oss << match_index_[i];
    if (i + 1 != match_index_.size()) {
      oss << ", ";
    }
  }
  oss << "]\n";

  oss << "heartbeats_sent_this_term_: " << heartbeats_sent_this_term_ << "\n";
  oss << "last_log_index_: " << last_log_index_ << "\n";
  oss << "commit_index_: " << commit_index_ << "\n";
  oss << "last_committed_: " << last_committed_ << "\n";
  oss << "role_: " << static_cast<int>(role_) << "\n";

  oss << "votes_: [";
  for (size_t i = 0; i < votes_.size(); ++i) {
    oss << votes_[i];
    if (i + 1 != votes_.size()) {
      oss << ", ";
    }
  }
  oss << "]\n";

  oss << "--------------------------";

  LOG(INFO) << oss.str();
}

}  // namespace raft
}  // namespace resdb
