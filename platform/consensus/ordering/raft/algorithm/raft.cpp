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

using SnapshotQueueItem = std::pair<uint64_t, size_t>;

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

static std::future<void> MakeReadyFuture() {
  std::promise<void> promise;
  promise.set_value();
  return promise.get_future();
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
           ReplicaCommunicator* replica_communicator, RaftRecovery* recovery,
           const ResDBConfig& config)
    : ProtocolBase(id, f, total_num),
      current_term_(0),
      voted_for_(-1),
      heartbeats_sent_this_term_(0),
      last_log_index_(-1),  // This value is unsigned, but after the sentinel is
                            // added wraps back around to 0.
      commit_index_(0),
      last_committed_(0),
      role_(Role::FOLLOWER),
      current_leader_(1),
      snapshot_last_index_(0),
      snapshot_last_term_(0),
      truncated_last_index_(0),
      truncated_last_term_(0),
      drain_requested_(false),
      is_stop_(false),
      quorum_((total_num / 2) + 1),
      verifier_(verifier),
      leader_election_manager_(leader_election_manager),
      replica_communicator_(replica_communicator),
      recovery_(recovery),
      config_(config) {
  assert(recovery_);
  id_ = id;
  total_num_ = total_num;
  f_ = (total_num - 1) / 2;

  // Derive snapshot file paths from the same directory as the WAL/metadata.
  // recovery_->GetFilePath() returns e.g. "./wal_log/log", so we use its
  // parent directory.
  std::string wal_dir = recovery_->GetWalDir();
  snapshot_file_path_ = wal_dir + "/snapshot.dat";
  snapshot_tmp_path_ = wal_dir + "/snapshot.dat.tmp";

  LogEntry sentinel;
  sentinel.entry.set_term(0);
  sentinel.entry.set_command("COMMON_PREFIX");
  {
    std::lock_guard<std::mutex> lk(mutex_);
    AddToLogLocked(sentinel, false);
    last_log_index_ = 0;

    progress_.resize(total_num_ + 1);
    for (size_t i = 1; i <= static_cast<size_t>(total_num_); ++i) {
      progress_[i].next_index = last_log_index_ + 1;
      progress_[i].match_index = last_log_index_;
      progress_[i].state = ProgressState::REPLICATE;
    }

    snapshot_send_time_.assign(
        total_num_ + 1,
        std::make_pair(std::chrono::steady_clock::time_point{}, size_t{0}));

    if (config_.GetConfigData().recovery_enabled()) {
      snapshot_sending_thread_ =
          std::thread([this] { this->CheckSnapshotQueue(); });
    }
    if (id_ == 1) {
      SetRoleLocked(Role::LEADER);
    }

    if (config_.GetConfigData().has_raft_follower_batch_timeout_ms()) {
      auto timeout = config_.GetConfigData().raft_follower_batch_timeout_ms();
      if (timeout == 0) {
        enable_batching_ = false;
      }
      batch_threshold_ = std::chrono::milliseconds(timeout);
    }
  }
}

void Raft::CheckSnapshotQueue() {
  while (true) {
    bool should_drain = false;
    {
      std::unique_lock<std::mutex> lk(snapshot_queue_mutex_);

      snapshot_queue_cv_.wait(lk, [this] {
        return is_stop_ || drain_requested_ || !snapshot_queue_.Empty();
      });

      if (is_stop_) {
        return;
      }
      should_drain = drain_requested_;
      drain_requested_ = false;
    }

    if (should_drain) {
      while (snapshot_queue_.Pop()) {
      }
      continue;
    }

    while (!snapshot_queue_.Empty() && !IsStop()) {
      auto element = snapshot_queue_.Pop();
      auto follower = element->first;
      auto byte_offset = element->second;

      SendInstallSnapshot(follower, byte_offset);
    }
  }
}

// Requires the raft mutex to be held.
// Returns true if it is appropriate to send a snapshot chunk to follower_id
// at byte_offset. Sending is allowed when:
//   - No snapshot has been sent recently (time_point is at epoch), OR
//   - The byte_offset is strictly greater than the last-sent offset (progress),
//     OR
//   - The last-sent offset matches but the deadline has elapsed (retry).
bool Raft::ShouldSendSnapshotChunkLocked(int follower_id,
                                         size_t byte_offset) const {
  const auto& [last_sent_time, last_sent_offset] =
      snapshot_send_time_[follower_id];

  if (last_sent_time == std::chrono::steady_clock::time_point{}) {
    return true;
  }

  if (byte_offset > last_sent_offset) {
    return true;
  }

  auto elapsed = std::chrono::steady_clock::now() - last_sent_time;
  return elapsed >= snapshot_response_deadline_;
}

void Raft::EnqueueSnapshot(int follower_id, size_t byte_offset) {
  std::lock_guard<std::mutex> lk(mutex_);
  EnqueueSnapshotLocked(follower_id, byte_offset);
}

void Raft::EnqueueSnapshotLocked(int follower_id, size_t byte_offset) {
  if (!ShouldSendSnapshotChunkLocked(follower_id, byte_offset)) {
    return;
  }

  LOG(INFO) << "Enqueuing snapshot for follower id: " << follower_id;

  // In order to prevent multiple threads from queueing the same snapshot, set
  // the send time now, and again in SendInstallSnapshot once it is actually
  // sent.
  snapshot_send_time_[follower_id] =
      std::make_pair(std::chrono::steady_clock::now(), byte_offset);

  snapshot_queue_.Push(
      std::make_unique<SnapshotQueueItem>(follower_id, byte_offset));
  snapshot_queue_cv_.notify_one();
}

void Raft::RequestSnapshotQueueDrain() {
  {
    std::lock_guard<std::mutex> lk(snapshot_queue_mutex_);
    drain_requested_ = true;
  }
  snapshot_queue_cv_.notify_one();
}

Raft::~Raft() {
  {
    std::lock_guard<std::mutex> lk(snapshot_queue_mutex_);
    is_stop_ = true;
  }

  snapshot_queue_cv_.notify_all();
  if (snapshot_sending_thread_.joinable()) {
    snapshot_sending_thread_.join();
  }
}

bool Raft::IsStop() {
  std::lock_guard<std::mutex> lk(snapshot_queue_mutex_);
  return is_stop_;
}

void Raft::SetRoleLocked(Role role) { role_ = role; }

void Raft::SetRole(Role role) {
  std::lock_guard<std::mutex> lk(mutex_);
  role_ = role;
}

bool Raft::ReceiveTransaction(std::unique_ptr<Request> req) {
  std::string serialized;
  if (!req->SerializeToString(&serialized)) {
    LOG(ERROR) << __FUNCTION__ << ": req could not be serialized";
    return false;
  }
  bool direct_to_leader = false;
  bool broadcasted = false;
  std::future<void> wal_future;
  uint64_t my_log_index = 0;

  std::vector<AeFields> messages;
  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ != Role::LEADER) {
      VLOG(1) << __FUNCTION__
              << ": Replica is not leader, redirecting clients to leader";
      direct_to_leader = true;
      return;
    }
    // Append new transaction to log.
    LogEntry log_entry;
    log_entry.entry.set_term(current_term_);

    log_entry.entry.set_command(std::move(serialized));
    log_entry.GetSerializedSize();
    wal_future = AddToLogLocked(log_entry);
    my_log_index = last_log_index_;

    progress_[id_].next_index = last_log_index_ + 1;
    // Match index is set later once wal_future has shown it has persisted to
    // disk.

    VLOG(2) << ": Leader appended entry at index " << last_log_index_;

    // Prepare fields for AppendEntries message.
    PruneExpiredInFlightMsgsLocked();
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_batch = now - timestamp_since_last_transaction_batch_;
    if (time_since_last_batch > batch_threshold_ || !enable_batching_) {
      broadcasted = true;
      timestamp_since_last_transaction_batch_ = now;
      messages = GatherAeFieldsForBroadcastLocked();
      for (const auto& msg : messages) {
        RecordNewInFlightMsgLocked(msg, now);
      }
    }

    // Detect if batches are consistently hitting large sizes
    for (const auto& msg : messages) {
      VLOG_IF(1, msg.entries.size() >= 100)
          << "Large batch broadcasted to follower " << msg.follower_id
          << " with " << msg.entries.size() << " entries";
    }
  }();

  if (direct_to_leader) {
    DirectToLeader dtl;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      dtl.set_leader_id(current_leader_);
    }
    for (const auto& client : replica_communicator_->GetClientReplicas()) {
      int id = client.id();
      SendMessage(DirectToLeaderMsg, dtl, id);
    }
    return false;
  }
  for (const auto& msg : messages) {
    CreateAndSendAppendEntryMsg(msg);
  }
  if (broadcasted) {
    leader_election_manager_->OnAeBroadcast();
  }

  // Entries can be sent out before they are persisted to disk. This check
  // logically could be delayed up until the leader checks for quorum to be met
  // on an entry, but this should be sufficient to reduce latency.
  if (wal_future.valid()) {
    wal_future.wait();
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ == Role::LEADER) {
      progress_[id_].match_index =
          std::max(progress_[id_].match_index, my_log_index);
    }
  }

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
  uint64_t conflicting_index = 0;
  uint64_t conflicting_term = 0;
  auto leader_commit = ae->leader_commit_index();
  auto leader_id = ae->leader_id();
  std::vector<std::unique_ptr<Request>> entries_to_apply;
  std::future<void> wal_future;

  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;
    last_log_index = last_log_index_;
    uint64_t entries_size = static_cast<uint64_t>(ae->entries_size());
    VLOG_IF(2, entries_size > 0)
        << __FUNCTION__ << ": follower received " << entries_size
        << " entries from " << leader_id << " with prevlogindex "
        << ae->prev_log_index() << " while having last_log_index_ "
        << last_log_index_ << " first_entry_term " << ae->entries(0).term();

    tr = TermCheckLocked(ae->term());
    if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(ae->term());
    } else if (role_ != Role::FOLLOWER && tr == TermRelation::CURRENT) {
      demoted = DemoteSelfLocked(ae->term());
    }

    if (tr != TermRelation::STALE) {
      current_leader_ = leader_id;
      if (role_ == Role::FOLLOWER) {
        uint64_t i = ae->prev_log_index();

        if (i <= truncated_last_index_ ||
            (i < static_cast<uint64_t>(GetLogicalLogSize()) &&
             ae->prev_log_term() == GetLogTermAtIndex(i))) {
          success = true;
        }
      }
    }
    term = current_term_;
    // Early return if we should not append.
    if (!success) {
      auto prev_log_index = ae->prev_log_index();
      bool log_too_short = ae->prev_log_index() > last_log_index_;
      bool has_conflicting_term_at_index =
          !log_too_short &&
          (ae->prev_log_term() != GetLogTermAtIndex(prev_log_index));
      if (has_conflicting_term_at_index) {
        conflicting_term = GetLogTermAtIndex(prev_log_index);
        conflicting_index = prev_log_index;

        // Need to ensure that we don't try to access an element that has been
        // truncated. Also, any element that has been committed must be in the
        // leader's log, so it is unnecessary to check.
        bool entry_to_check_not_committed =
            conflicting_index - 1 >= commit_index_;
        while (entry_to_check_not_committed &&
               GetLogTermAtIndex(conflicting_index - 1) == conflicting_term) {
          conflicting_index--;
          entry_to_check_not_committed = conflicting_index - 1 >= commit_index_;
        }
      }
      return;
    }

    VLOG_IF(1, entries_size > 0 &&
                   (ae->prev_log_index() + entries_size) <= last_log_index_)
        << "Redundant AppendEntries received. prevLogIndex: "
        << ae->prev_log_index() << " + size: " << entries_size
        << " <= follower last_log_index_: " << last_log_index_;

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

    // Check for conflicting entry terms in existing indices.
    // If there is a conflict, delete the suffix and short circuit out of the
    // loop.
    while (log_idx < GetLogicalLogSize() && entries_idx < entries_size) {
      uint64_t log_term = ae->entries(entries_idx).term();
      if (log_term != GetLogTermAtIndex(log_idx)) {
        TruncateLogLocked(log_idx);

        VLOG(1) << __FUNCTION__ << ": follower saw term mismatch at index "
                << log_idx << ". Suffix erased from log";

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
    wal_future = AddToLogLocked(std::move(log_entries_to_add));
    last_log_index = last_log_index_;

    VLOG_IF(2, (append_size > 1) && last_log_index_ >= firstAppend_idx)
        << __FUNCTION__ << ": follower appended entries at indices "
        << firstAppend_idx << " to " << last_log_index_;
    VLOG_IF(2, (append_size == 1) && last_log_index_ >= firstAppend_idx)
        << __FUNCTION__ << ": follower appended entry at index "
        << last_log_index_;

    // Try to raise commit_index and commit entries
    uint64_t prev_commit_index = commit_index_;
    if (leader_commit > commit_index_) {
      commit_index_ = std::min(leader_commit, last_log_index_);

      VLOG_IF(2, commit_index_ > prev_commit_index)
          << __FUNCTION__ << ": Raised commit_index_ from " << prev_commit_index
          << " to " << commit_index_;
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

  VLOG_IF(2, !entries_to_apply.empty())
      << "Follower applying " << entries_to_apply.size()
      << " committed entries starting at "
      << last_log_index - entries_to_apply.size();
  for (auto& entry : entries_to_apply) {
    commit_(*entry);
  }

  // Wait until all log entries have been persisted to disk before responding.
  if (wal_future.valid()) {
    wal_future.wait();
  }

  AppendEntriesResponse aer;
  aer.set_term(term);
  aer.set_success(success);
  aer.set_id(id_);
  aer.set_last_log_index(last_log_index);
  VLOG(3) << "sending aer Success: " << (success ? "true" : "false")
          << " last log index: " << last_log_index;
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
  std::vector<AeFields> fields_vector;
  int follower_id = aer->id();
  bool should_send_snapshot = false;
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

    // If successful, update match_index and try to commit more entries.
    if (aer->success()) {
      VLOG(3) << "Before update: AER success from follower " << follower_id
              << " their last_log_index=" << aer->last_log_index()
              << " progress_[follower].match_index="
              << progress_[follower_id].match_index
              << " progress_[follower].next_index="
              << progress_[follower_id].next_index;
      // A success behind the follower's match_index must be stale, so it can be
      // ignored. match_index must never decrease.
      if (aer->last_log_index() < progress_[follower_id].match_index) {
        return;
      }

      progress_[follower_id].match_index =
          std::min(aer->last_log_index(), last_log_index_);

      progress_[follower_id].next_index =
          std::max(progress_[follower_id].next_index,
                   progress_[follower_id].match_index + 1);
      if (progress_[follower_id].state == ProgressState::PROBE) {
        assert(aer->last_log_index() <= last_log_index_);
        // If the follower was being probed, we cannot trust the old next_index.
        progress_[follower_id].next_index = std::max(
            progress_[follower_id].match_index + 1, aer->last_log_index() + 1);
        progress_[follower_id].state = ProgressState::REPLICATE;
        progress_[follower_id].probe_in_flight = false;
      }

      VLOG(1) << "AER success from follower " << follower_id
              << " their last_log_index=" << aer->last_log_index()
              << " progress_[follower].match_index="
              << progress_[follower_id].match_index
              << " progress_[follower].next_index="
              << progress_[follower_id].next_index;
      // Use the updated match_index to find new entries eligible for commit.
      std::vector<uint64_t> sorted;
      sorted.reserve(progress_.size());
      for (const auto& follower_progress : progress_) {
        sorted.push_back(follower_progress.match_index);
      }
      std::sort(sorted.begin(), sorted.end(), std::greater<uint64_t>());
      uint64_t last_replicated_index = sorted[quorum_ - 1];
      // Need to check the last_replicated_index contains entry from current
      // term.
      // If this node recently became leader and does not have an up-to-date
      // matchIndex, then
      if (last_replicated_index > commit_index_ &&
          last_replicated_index >= truncated_last_index_ &&
          GetLogTermAtIndex(last_replicated_index) == current_term_) {
        VLOG(2) << __FUNCTION__ << ": Raised commit_index_ from "
                << commit_index_ << " to " << last_replicated_index;
        commit_index_ = last_replicated_index;
      }
      VLOG(1) << "Quorum check: last_replicated_index=" << last_replicated_index
              << " commit_index_=" << commit_index_ << " term_at_replicated="
              << (last_replicated_index >= truncated_last_index_
                      ? GetLogTermAtIndex(last_replicated_index)
                      : 0)
              << " current_term_=" << current_term_;

      // Apply any newly committed entries to state machine.
      entries_to_apply = PrepareCommitLocked();
    }

    if (!aer->success() ||
        (progress_[follower_id].next_index < last_log_index_ + 1)) {
      if (!aer->success()) {
        // If we do not get a success, set the follower's state to probe, send
        // out one AppendEntries to figure out where it is at, and wait for a
        // response. Once we have identified the correct location to start
        // sending catch-up entries, that will be on the success path.
        // Otherwise, stale probes will be re-attempted on heartbeats once they
        // have expired.
        FollowerProgress& follower_progress = progress_[follower_id];
        if (follower_progress.state == ProgressState::REPLICATE) {
          follower_progress.state = ProgressState::PROBE;
          follower_progress.in_flight.clear();
        }
        // If conflicting_index and conflicting_term are 0, that means the
        // follower's log was just too short. If not, that means it had an entry
        // with a conflicting term.
        auto index_to_send = aer->last_log_index();
        auto conflicting_term = aer->conflicting_term();
        if (conflicting_term) {
          index_to_send = aer->conflicting_index();
          assert(index_to_send > 0);
          assert(index_to_send <= last_log_index_);

          // Since a leader starts a term by committing a no-op, we don't need
          // to worry about advancing too far.
          assert(conflicting_term < current_term_);
          // If we have no entry at that term, then the follower's entire log
          // starting at that term needs to be replaced. If we do have an entry
          // at that term, advance until the start of the next term.
          bool is_entry_with_matching_term =
              GetLogTermAtIndex(index_to_send) == conflicting_term;
          while (is_entry_with_matching_term) {
            index_to_send++;
            assert(index_to_send <= last_log_index_);
            is_entry_with_matching_term =
                GetLogTermAtIndex(index_to_send) == conflicting_term;
          }
          index_to_send--;
        }
        follower_progress.next_index =
            std::max(std::min(index_to_send + 1, last_log_index_ + 1),
                     follower_progress.match_index + 1);
        // CanSendLocked will allow exactly one probe, then block.
        VLOG(1) << "AppendEntriesResponse indicates FAILURE from follower "
                << follower_id
                << " next_index is: " << progress_[follower_id].next_index
                << " their last_log_index is: " << aer->last_log_index();
      }
      if (aer->last_log_index() < truncated_last_index_) {
        should_send_snapshot = true;
      } else {
        bool follower_is_being_probed =
            progress_[follower_id].state == ProgressState::PROBE;
        while (CanSendLocked(follower_id)) {
          AeFields fields = GatherAeFieldsLocked(follower_id);
          if (fields.entries.empty() && !follower_is_being_probed) {
            break;
          }
          resending = true;
          auto now = std::chrono::steady_clock::now();
          RecordNewInFlightMsgLocked(fields, now);
          fields_vector.push_back(std::move(fields));
        }
        resending = !fields_vector.empty();
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
    for (auto& fields : fields_vector) {
      CreateAndSendAppendEntryMsg(fields);
    }
  }
  if (should_send_snapshot) {
    EnqueueSnapshot(follower_id, 0);
  }

  VLOG_IF(2, !entries_to_apply.empty())
      << "Leader applying " << entries_to_apply.size() << " committed entries";
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
      SetVotedForLocked(rv_sender);
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
    LOG(INFO) << __FUNCTION__ << ": Replica " << voter_id
              << " voted for me. Votes: " << votes_.size() << "/" << quorum_
              << " in term " << current_term_;
    if (votes_.size() >= quorum_) {
      elected = true;
      SetRoleLocked(Role::LEADER);
      ClearInFlightsLocked();

      for (size_t i = 1; i <= static_cast<size_t>(total_num_); ++i) {
        progress_[i] = FollowerProgress{};
        progress_[i].next_index = last_log_index_ + 1;
        progress_[i].match_index = 0;
      }

      snapshot_send_time_.assign(
          total_num_ + 1,
          std::make_pair(std::chrono::steady_clock::time_point{}, size_t{0}));
      VLOG(1) << "Post-election index state: last_log_index_="
              << last_log_index_
              << " progress_[id_].next_index=" << progress_[id_].next_index
              << " progress_[id_].match_index=" << progress_[id_].match_index;
      LOG(INFO) << __FUNCTION__ << ": CANDIDATE->LEADER in term "
                << current_term_;

      LogEntry noop;
      noop.entry.set_term(current_term_);
      noop.entry.set_command("RAFT_NO_OP");
      AddToLogLocked(noop);
      progress_[id_].next_index = last_log_index_ + 1;
      progress_[id_].match_index = last_log_index_;
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
    SetCurrentTermAndVotedForLocked(current_term_ + 1, id_);
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
  std::vector<uint64_t> send_snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ != Role::LEADER) {
      LOG(WARNING) << __FUNCTION__ << ": Non-Leader tried to start HeartBeat";
      return;
    }
    current_term = current_term_;

    heartbeats_sent_this_term_++;
    heartBeat_num = heartbeats_sent_this_term_;

    PruneExpiredInFlightMsgsLocked();

    bool heartbeat = true;
    messages = GatherAeFieldsForBroadcastLocked(heartbeat);

    auto now = std::chrono::steady_clock::now();
    for (const auto& msg : messages) {
      RecordNewInFlightMsgLocked(msg, now);
    }

    for (size_t i = 1; i <= total_num_; ++i) {
      if (i == id_) {
        continue;
      }
      if (progress_[i].next_index <= truncated_last_index_) {
        send_snapshot.push_back(i);
      }
    }
  }

  auto msg_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration msg_delta;

  for (const auto& msg : messages) {
    CreateAndSendAppendEntryMsg(msg);
  }

  leader_election_manager_->OnAeBroadcast();

  auto msg_end = std::chrono::steady_clock::now();
  msg_delta = msg_end - msg_start;
  auto msg_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(msg_delta).count();

  for (auto follower_id : send_snapshot) {
    const auto& [last_sent_time, last_sent_offset] =
        snapshot_send_time_[follower_id];
    // EnqueueSnapshot checks ShouldSendSnapshotChunkLocked internally,
    // so repeated heartbeats will only re-trigger after the deadline expires
    // for the same chunk, or immediately for any new chunk offset.
    EnqueueSnapshot(follower_id, last_sent_offset);
  }

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
    SetCurrentTermAndVotedForLocked(term, -1);
  }
  if (role_ != Role::FOLLOWER) {
    RequestSnapshotQueueDrain();
    snapshot_send_time_.assign(
        total_num_ + 1,
        std::make_pair(std::chrono::steady_clock::time_point{}, size_t{0}));

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
  if (last_log_index_ <= truncated_last_index_) {
    return truncated_last_term_;
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

    if (GetLogEntryAtIndex(last_committed_).entry.command() == "RAFT_NO_OP") {
      command->set_data("RAFT_NO_OP");
    } else if (!command->ParseFromString(
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
      VLOG(1) << __FUNCTION__ << ": Applying index entries " << begin << " to "
              << last_committed_;
    } else {
      VLOG(1) << __FUNCTION__ << ": Applying index entry " << last_committed_;
    }
  }

  return commit_vec;
}

// Requires the raft mutex to be held.
AeFields Raft::GatherAeFieldsLocked(int follower_id) {
  AeFields fields{};

  fields.term = current_term_;
  fields.leader_id = id_;
  fields.leader_commit = commit_index_;
  fields.prev_log_index = progress_[follower_id].next_index - 1;
  // The follower may be behind our truncated_last_index_, so we need to guard
  // against that. Even if this term is not correct, the follower will respond
  // failure regardless because it needs a snapshot.
  fields.prev_log_term = (fields.prev_log_index <= truncated_last_index_)
                             ? snapshot_last_term_
                             : GetLogTermAtIndex(fields.prev_log_index);
  fields.follower_id = follower_id;
  if (!CanSendLocked(follower_id) ||
      progress_[follower_id].state == ProgressState::PROBE) {
    return fields;
  }

  // If a follower is behind, still send entries to catch them up.
  uint32_t msg_bytes = max_header_bytes_;
  const uint64_t first_new = progress_[follower_id].next_index;
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
  VLOG(1) << "GatherAeFields for follower " << follower_id
          << " prev_log_index=" << fields.prev_log_index
          << " entries_count=" << fields.entries.size()
          << " next_index=" << progress_[follower_id].next_index
          << " last_log_index=" << last_log_index_;
  return fields;
}

// Any followers that are behind will be sent entries if they are under
// max_in_flight_per_follower_ and their next_index is behind the leader's
// last_log_index_. AeFields.entries will each contain at most max_entries_
// amount of entries. Followers at max_in_flight_per_follower_ will be ignored
// unless this is a heartbeat.
std::vector<AeFields> Raft::GatherAeFieldsForBroadcastLocked(bool heart_beat) {
  assert(role_ == Role::LEADER);
  std::vector<AeFields> fields_vec;
  fields_vec.reserve(total_num_ - 1);
  for (size_t i = 1; i <= total_num_; ++i) {
    if (i == id_) {
      continue;
    }
    if (!heart_beat && !CanSendLocked(i)) {
      VLOG(3) << "Skipping follower " << i
              << " (in-flight limit, queue=" << progress_[i].in_flight.size()
              << ", match=" << progress_[i].match_index
              << ", next=" << progress_[i].next_index
              << ", lag=" << (last_log_index_ - progress_[i].match_index)
              << ")";
      continue;
    }
    AeFields fields = GatherAeFieldsLocked(i);
    fields_vec.push_back(fields);
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

  VLOG_IF(2, fields.entries.size() > 1)
      << __FUNCTION__ << ": Sent AE with " << fields.entries.size()
      << (fields.entries.size() == 1 ? " entry" : " entries") << " to follower "
      << follower_id << " at prev_log_index index " << fields.prev_log_index;
  VLOG_IF(3, fields.entries.size() == 0)
      << __FUNCTION__ << ": Sent heartbeat to follower " << follower_id;
}

LogEntry Raft::CreateLogEntry(const Entry& entry) const {
  LogEntry new_entry;
  new_entry.entry = entry;
  return new_entry;
}

// Requires the raft mutex to be held.
void Raft::ClearInFlightsLocked() {
  assert(role_ == Role::LEADER);
  for (auto& follower_progress : progress_) {
    follower_progress.in_flight.clear();
  }
}

// Requires the raft mutex to be held.
void Raft::PruneExpiredInFlightMsgsLocked() {
  assert(role_ == Role::LEADER);
  auto now = std::chrono::steady_clock::now();
  for (size_t i = 1; i < progress_.size(); ++i) {
    if (i == id_) {
      continue;
    }
    auto& follower = progress_[i];
    auto& vec = follower.in_flight;
    if (vec.empty()) {
      continue;
    }
    bool any_expired = false;
    for (const auto& msg : vec) {
      if (now - msg.time_sent >= ae_response_deadline_) {
        any_expired = true;
        break;
      }
    }
    if (!any_expired) {
      continue;
    }
    // When any in-flight entry expires, clear all in-flights for this follower
    // and reset its state.
    vec.clear();
    follower.next_index = follower.match_index + 1;
    if (follower.next_index <= truncated_last_index_) {
      follower.state = ProgressState::SNAPSHOT;
      EnqueueSnapshotLocked(static_cast<int>(i), snapshot_send_time_[i].second);
    } else if (follower.state == ProgressState::REPLICATE) {
      follower.state = ProgressState::PROBE;
    }

    if (replication_logging_flag_) {
      LOG(INFO) << __FUNCTION__
                << ": Pruned all expired inflight AEs for follower " << i
                << ", reset to PROBE at next_index=" << follower.next_index;
    }
  }
}

void Raft::PruneRedundantInFlightMsgsLocked(int follower_id,
                                            uint64_t follower_last_log_index) {
  assert(role_ == Role::LEADER);
  assert(follower_id > 0);
  assert(follower_id != id_);

  auto& vec = progress_[follower_id].in_flight;
  vec.erase(std::remove_if(vec.begin(), vec.end(),
                           [&](const InFlightMsg& msg) {
                             return msg.last_index_of_segment_sent <=
                                    follower_last_log_index;
                           }),
            vec.end());
}

void Raft::RecordNewInFlightMsgLocked(
    const AeFields& msg, std::chrono::steady_clock::time_point timestamp) {
  int follower_id = msg.follower_id;
  if (msg.entries.empty()) {
    // Heartbeats are not stored in the in_flight, but must be sent as part of a
    // probe.
    if (progress_[follower_id].state == ProgressState::PROBE) {
      progress_[follower_id].probe_in_flight = true;
    }
    return;
  }
  InFlightMsg in_flight;
  in_flight.time_sent = timestamp;
  in_flight.prev_log_index_sent = msg.prev_log_index;
  in_flight.last_index_of_segment_sent =
      msg.prev_log_index + msg.entries.size();
  progress_[follower_id].in_flight.push_back(in_flight);

  progress_[follower_id].next_index = in_flight.last_index_of_segment_sent + 1;
}

// Requires the raft mutex to be held.
bool Raft::CanSendLocked(int follower_id) const {
  auto follower_progress = progress_[follower_id];
  assert(role_ == Role::LEADER);

  // Never send any entries to a follower who needs a snapshot.
  if (follower_progress.next_index <= truncated_last_index_) {
    return false;
  }

  switch (follower_progress.state) {
    case ProgressState::PROBE:
      // While probing, only allow one in flight message.
      return !follower_progress.probe_in_flight;
    case ProgressState::REPLICATE:
      // Only send an entry if the follower is under the in flight limit.
      return (follower_progress.in_flight.size() < max_in_flight_per_follower_);
    case ProgressState::SNAPSHOT:
      return false;
  }
  assert(false);
}

const LogEntry& Raft::GetLogEntryAtIndex(uint64_t index) const {
  assert(index > truncated_last_index_ &&
         "Tried to access entry that has been prefix truncated");
  // A sentinel value is always included after a snapshot.
  // Example: truncated_last_index_ = 5, we have truncated the entire log, added
  // 1 entry, then log_.size() == 2 with the sentinel. index could be 6, and
  // truncated_last_index_ + log_.size() == 7.
  assert(index < truncated_last_index_ + log_.size() &&
         "Tried to access element that has not been added yet");
  return log_[index - truncated_last_index_];
}

const uint64_t Raft::GetLogTermAtIndex(uint64_t index) const {
  assert(index >= truncated_last_index_ &&
         "Tried to access entry that has been prefix truncated");
  assert(index < truncated_last_index_ + log_.size() &&
         "Tried to access element that has not been added yet");
  if (index == truncated_last_index_) {
    return truncated_last_term_;
  }

  return log_[index - truncated_last_index_].entry.term();
}

// This would be what log_.size() returns if no prefix truncation occurred.
uint64_t Raft::GetLogicalLogSize() const {
  return log_.size() + truncated_last_index_;
}

void Raft::SetCurrentTerm(uint64_t current_term, bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  SetCurrentTermLocked(current_term, write_metadata);
}

// Requires the raft mutex to be held.
void Raft::SetCurrentTermLocked(uint64_t current_term, bool write_metadata) {
  LOG(INFO) << "Updating term from " << current_term_ << " to " << current_term;
  current_term_ = current_term;
  if (write_metadata) {
    WriteMetadataLocked();
  }
}

void Raft::SetVotedFor(int voted_for, bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  SetVotedForLocked(voted_for, write_metadata);
}

// Requires the raft mutex to be held.
void Raft::SetVotedForLocked(int voted_for, bool write_metadata) {
  voted_for_ = voted_for;
  if (write_metadata) {
    WriteMetadataLocked();
  }
}

void Raft::SetCurrentTermAndVotedFor(uint64_t current_term, int voted_for,
                                     bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  SetCurrentTermAndVotedForLocked(current_term, voted_for, write_metadata);
}

// Requires the raft mutex to be held.
void Raft::SetCurrentTermAndVotedForLocked(uint64_t current_term, int voted_for,
                                           bool write_metadata) {
  LOG(INFO) << "Updating term from " << current_term_ << " to " << current_term;
  current_term_ = current_term;
  voted_for_ = voted_for;
  if (write_metadata) {
    WriteMetadataLocked();
  }
}

// Requires the raft mutex to be held.
void Raft::SetSnapshotLastIndexAndTermLocked(uint64_t snapshot_last_index,
                                             uint64_t snapshot_last_term,
                                             uint64_t truncated_last_index,
                                             uint64_t truncated_last_term,
                                             bool write_metadata) {
  snapshot_last_index_ = snapshot_last_index;
  snapshot_last_term_ = snapshot_last_term;
  truncated_last_index_ = truncated_last_index;
  truncated_last_term_ = truncated_last_term;
  assert(log_.size() >= 1);
  log_[0].entry.set_term(truncated_last_term_);
  LOG(INFO) << "setting snapshot_last_index " << snapshot_last_index
            << " and snapshot_last_term" << snapshot_last_term;
  if (write_metadata) {
    WriteMetadataLocked();
    return;
  }

  // Function is only called with write_metadata == false on initial recovery,
  // so these variables need to be set.
  last_log_index_ = truncated_last_index_;
  commit_index_ = truncated_last_index_;
  last_committed_ = truncated_last_index_;
}

void Raft::SetSnapshotLastIndexAndTerm(uint64_t snapshot_last_index,
                                       uint64_t snapshot_last_term,
                                       bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  SetSnapshotLastIndexAndTermLocked(snapshot_last_index, snapshot_last_term,
                                    snapshot_last_index, snapshot_last_term,
                                    write_metadata);
}

uint64_t Raft::GetSnapshotLastIndex() { return snapshot_last_index_; }

// Requires the raft mutex to be held.
void Raft::WriteMetadataLocked() {
  recovery_->WriteMetadata(current_term_, voted_for_, snapshot_last_index_,
                           snapshot_last_term_);
}

void Raft::AddToLog(LogEntry& log_entry_to_add, bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  AddToLogLocked(log_entry_to_add, write_metadata);
}

// Requires the raft mutex to be held.
std::future<void> Raft::AddToLogLocked(LogEntry& log_entry_to_add,
                                       bool write_metadata) {
  last_log_index_++;
  Entry* entry;
  entry = &log_entry_to_add.entry;
  std::future<void> wal_future;
  if (write_metadata) {
    wal_future = recovery_->AddLogEntry(entry, last_log_index_);
  } else {
    wal_future = MakeReadyFuture();
  }
  log_.push_back(log_entry_to_add);
  assert(last_log_index_ == GetLogicalLogSize() - 1);
  return wal_future;
}

void Raft::AddToLog(std::vector<LogEntry> log_entries_to_add,
                    bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  AddToLogLocked(log_entries_to_add, write_metadata);
}

// Requires the raft mutex to be held.
std::future<void> Raft::AddToLogLocked(std::vector<LogEntry> log_entries_to_add,
                                       bool write_metadata) {
  if (log_entries_to_add.empty()) {
    return MakeReadyFuture();
  }

  std::future<void> wal_future;
  if (write_metadata) {
    std::vector<Entry> entries;
    entries.reserve(log_entries_to_add.size());
    for (const auto& log_entry : log_entries_to_add) {
      entries.push_back(log_entry.entry);
    }
    wal_future = recovery_->AddLogEntry(entries, last_log_index_ + 1);
  } else {
    wal_future = MakeReadyFuture();
  }

  last_log_index_ += log_entries_to_add.size();
  log_.reserve(log_.size() + log_entries_to_add.size());
  log_.insert(log_.end(), std::make_move_iterator(log_entries_to_add.begin()),
              std::make_move_iterator(log_entries_to_add.end()));

  assert(last_log_index_ == GetLogicalLogSize() - 1);
  return wal_future;
}

void Raft::TruncateLog(uint64_t first_index, bool write_metadata) {
  std::lock_guard<std::mutex> lk(mutex_);
  TruncateLogLocked(first_index, write_metadata);
}

// Requires the raft mutex to be held.
void Raft::TruncateLogLocked(uint64_t first_index, bool write_metadata) {
  assert(first_index > commit_index_);
  assert(first_index <= last_log_index_);
  auto first = log_.begin() + (first_index - truncated_last_index_);
  auto last = log_.begin() + (last_log_index_ - truncated_last_index_) + 1;
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

void Raft::TruncatePrefix(uint64_t snapshot_index) {
  std::lock_guard<std::mutex> lk(mutex_);
  TruncatePrefixLocked(snapshot_index);
}

// Requires the raft mutex to be held.
void Raft::TruncatePrefixLocked(uint64_t snapshot_index) {
  uint64_t index = (snapshot_index > snapshot_buffer_amount_)
                       ? std::max(snapshot_index - snapshot_buffer_amount_,
                                  truncated_last_index_)
                       : truncated_last_index_;

  if (index <= truncated_last_index_) {
    VLOG(1) << "Snapshot contains up to: " << snapshot_index
            << " which combined with snapshot_buffer_amount_: "
            << snapshot_buffer_amount_ << " is not more than "
            << truncated_last_index_ << ". No prefix truncation will occur.";
    return;
  }

  assert(index > truncated_last_index_ &&
         "Tried to truncate an entry that has been prefix truncated");
  assert(index <= last_committed_ &&
         "Tried to prefix truncate an element that has not been committed");
  LOG(INFO) << __FUNCTION__ << ": BEGIN index=" << index
            << " snapshot_last_index_=" << snapshot_last_index_
            << " truncated_last_index_=" << truncated_last_index_
            << " last_log_index_=" << last_log_index_
            << " commit_index_=" << commit_index_
            << " log_.size()=" << log_.size();

  // Keep the sentinel, erase everything up to the index.
  auto erase_end = log_.begin() + (index - truncated_last_index_);
  auto last_truncated_entry_term = GetLogTermAtIndex(index);
  auto last_snapshot_entry_term = GetLogTermAtIndex(snapshot_index);
  log_.erase(log_.begin() + 1, erase_end + 1);

  auto t1 = std::chrono::steady_clock::now();

  SetSnapshotLastIndexAndTermLocked(snapshot_index, last_snapshot_entry_term,
                                    index, last_truncated_entry_term);

  auto t2 = std::chrono::steady_clock::now();
  LOG(INFO)
      << __FUNCTION__ << ": SetSnapshotLastIndexAndTerm completed in "
      << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()
      << "ms";

  assert(last_log_index_ == GetLogicalLogSize() - 1);

  if (role_ != Role::LEADER) {
    return;
  }

  for (size_t i = 1; i <= static_cast<size_t>(total_num_); ++i) {
    if (static_cast<int>(i) == id_) {
      continue;
    }
    bool follower_behind_truncation =
        progress_[i].next_index - 1 < truncated_last_index_;
    LOG_IF(WARNING, follower_behind_truncation)
        << __FUNCTION__ << ": AFTER TRUNCATION follower " << i
        << " has next_index_=" << progress_[i].next_index
        << " which is now BEHIND truncated_last_index_="
        << truncated_last_index_
        << ". Snapshot transfer will only trigger on next heartbeat or AERs.";
    if (follower_behind_truncation) {
      progress_[i].state = ProgressState::SNAPSHOT;
      const auto& [last_sent_time, last_sent_offset] = snapshot_send_time_[i];
      EnqueueSnapshotLocked(static_cast<int>(i), last_sent_offset);
    }
    VLOG(1) << __FUNCTION__ << ": follower " << i
            << " next_index_=" << progress_[i].next_index
            << " match_index_=" << progress_[i].match_index
            << " behind_truncation=" << follower_behind_truncation;
  }
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
  // Flush storage to disk before WriteMetadataLocked().
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

  // Record the send time and offset before sending. If we are no longer the
  // leader by the time SendMessage returns, DemoteSelfLocked will have already
  // reset snapshot_send_time_ for all followers.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (role_ != Role::LEADER) {
      return;
    }
    snapshot_send_time_[follower_id] =
        std::make_pair(std::chrono::steady_clock::now(), byte_offset);
    progress_[follower_id].state = ProgressState::SNAPSHOT;
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
  TermRelation tr;
  InstallSnapshotResponse isr;
  // Copies of the pending snapshot's fd and path, taken under the lock so the
  // out-of-lock write/fsync lambdas never dereference a pointer into the map.
  int pending_fd = -1;
  std::string pending_tmp_path;
  size_t remaining = 0;
  std::string tmp_path_to_rename;
  bool ready_to_respond = false;

  // Closes and unlinks a pending snapshot's temp file, then erases the map
  // entry. Must be called with mutex_ held.
  auto AbortPendingLocked = [&](std::map<int, PendingSnapshot>::iterator it) {
    LOG(INFO) << "Aborting in-progress snapshot from leader " << it->first;
    if (it->second.fd >= 0) {
      close(it->second.fd);
    }
    if (!it->second.tmp_path.empty()) {
      unlink(it->second.tmp_path.c_str());
    }
    pending_snapshot_chunks_.erase(it);
  };

  // Same cleanup but for use outside the lock, operating on the copied fd and
  // path rather than the map entry (which may have been mutated by then).
  auto AbortPendingUnlocked = [&]() {
    if (pending_fd >= 0) {
      close(pending_fd);
      pending_fd = -1;
    }
    if (!pending_tmp_path.empty()) {
      unlink(pending_tmp_path.c_str());
      pending_tmp_path.clear();
    }
    std::lock_guard<std::mutex> lk(mutex_);
    pending_snapshot_chunks_.erase(leader_id);
  };

  [&]() {
    std::lock_guard<std::mutex> lk(mutex_);
    our_term = current_term_;

    tr = TermCheckLocked(is->term());
    if (tr == TermRelation::STALE) {
      LOG(INFO) << "ReceiveInstallSnapshot: stale term " << is->term()
                << " (ours=" << our_term << ")";
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(false);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      ready_to_respond = true;
      return;
    }

    if (tr == TermRelation::NEW) {
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
        isr.set_term(our_term);
        isr.set_id(id_);
        isr.set_need_snapshot(false);
        isr.set_bytes_stored(0);
        isr.set_last_included_index(last_included_index);
        isr.set_transfer_complete(false);
        ready_to_respond = true;
        return;
      }

      // Discard any in-progress transfer from this leader and start fresh.
      auto existing = pending_snapshot_chunks_.find(leader_id);
      if (existing != pending_snapshot_chunks_.end()) {
        AbortPendingLocked(existing);
      }

      // Open a new temp file for this snapshot transfer.
      std::string tmp_path = snapshot_file_path_ + ".recv.tmp";
      int fd = open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
      if (fd < 0) {
        LOG(ERROR) << "ReceiveInstallSnapshot: failed to open recv tmp file "
                   << tmp_path << ": " << strerror(errno);
        isr.set_term(our_term);
        isr.set_id(id_);
        isr.set_need_snapshot(true);
        isr.set_bytes_stored(0);
        isr.set_last_included_index(last_included_index);
        isr.set_transfer_complete(false);
        ready_to_respond = true;
        return;
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
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      ready_to_respond = true;
      return;
    }

    // Restart if this chunk belongs to a different snapshot.
    if (it->second.last_included_index != last_included_index ||
        it->second.last_included_term != last_included_term) {
      LOG(WARNING) << "ReceiveInstallSnapshot: chunk belongs to different "
                      "snapshot (index "
                   << last_included_index << " vs pending "
                   << it->second.last_included_index << "); requesting restart";
      AbortPendingLocked(it);
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      ready_to_respond = true;
      return;
    }

    // Reject out-of-order chunks and tell the leader what offset we expect
    // next.
    if (incoming_offset != it->second.expected_offset) {
      LOG(WARNING) << "ReceiveInstallSnapshot: out-of-order chunk: expected "
                   << it->second.expected_offset << " got " << incoming_offset;
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(it->second.expected_offset);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      ready_to_respond = true;
      return;
    }

    // Copy fd and path out of the map entry so the write/fsync lambdas below
    // can operate without holding the lock or dereferencing a map pointer.
    pending_fd = it->second.fd;
    pending_tmp_path = it->second.tmp_path;
    // Set up remaining so the write lambda knows how much data to write.
    remaining = is->data().size();

    if (done) {
      should_install = true;
    }
  }();

  // Write the chunk data to the temp file outside the lock. pending_fd and
  // pending_tmp_path were copied from the map entry under the lock above.
  [&]() {
    if (remaining == 0) {
      return;
    }
    LOG(INFO) << "Writing snapshot chunk to file: " << pending_tmp_path;
    const std::string& chunk = is->data();
    const char* ptr = chunk.data();
    while (remaining > 0) {
      ssize_t written = write(pending_fd, ptr, remaining);
      if (written <= 0) {
        LOG(ERROR) << "ReceiveInstallSnapshot: write to temp file failed: "
                   << strerror(errno);
        AbortPendingUnlocked();
        isr.set_term(our_term);
        isr.set_id(id_);
        isr.set_need_snapshot(true);
        isr.set_bytes_stored(0);
        isr.set_last_included_index(last_included_index);
        isr.set_transfer_complete(false);
        ready_to_respond = true;
        return;
      }
      ptr += written;
      remaining -= static_cast<size_t>(written);
    }
    if (fsync(pending_fd) < 0) {
      LOG(ERROR) << "ReceiveInstallSnapshot: fsync to temp file failed: "
                 << strerror(errno);
      // Non-fatal: data may not be durable but we can still proceed.
    }
    // Update expected_offset in the map under the lock.
    {
      std::lock_guard<std::mutex> lk(mutex_);
      auto it = pending_snapshot_chunks_.find(leader_id);
      if (it != pending_snapshot_chunks_.end()) {
        it->second.expected_offset += static_cast<uint64_t>(chunk.size());
        bytes_stored = it->second.expected_offset;
      }
    }
  }();

  // fsync, close, and rename the temp file once all chunks have arrived.
  [&]() {
    if (!done || ready_to_respond) {
      return;
    }
    if (fsync(pending_fd) < 0) {
      LOG(ERROR) << "ReceiveInstallSnapshot: final fsync failed: "
                 << strerror(errno);
      AbortPendingUnlocked();
      isr.set_term(our_term);
      isr.set_id(id_);
      isr.set_need_snapshot(true);
      isr.set_bytes_stored(0);
      isr.set_last_included_index(last_included_index);
      isr.set_transfer_complete(false);
      ready_to_respond = true;
      return;
    }
    LOG(INFO) << "Completed snapshot fsynced to disk";
    close(pending_fd);
    pending_fd = -1;
    tmp_path_to_rename = pending_tmp_path;
    // Erase the map entry now that we have everything we need from it.
    {
      std::lock_guard<std::mutex> lk(mutex_);
      pending_snapshot_chunks_.erase(leader_id);
    }
  }();

  if (demoted) {
    leader_election_manager_->OnRoleChange();
  }
  if (tr != TermRelation::STALE) {
    leader_election_manager_->OnHeartBeat();
  }

  if (should_install) {
    LOG(INFO) << "All snapshot chunks received, applying snapshot";
    {
      std::lock_guard<std::mutex> lk(mutex_);
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
      truncated_last_index_ = last_included_index;
      truncated_last_term_ = last_included_term;
    }

    if (rename(tmp_path_to_rename.c_str(), snapshot_file_path_.c_str()) < 0) {
      LOG(ERROR) << "ReceiveInstallSnapshot: rename failed: "
                 << strerror(errno);
    }

    std::string full_data;
    {
      int snap_fd = open(snapshot_file_path_.c_str(), O_RDONLY);
      if (snap_fd < 0) {
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
    std::lock_guard<std::mutex> lk(mutex_);
    WriteMetadataLocked();
    LOG(INFO) << "ReceiveInstallSnapshot: installed snapshot up to index="
              << last_included_index;
  }

  // Send final ACK.
  if (!ready_to_respond) {
    std::lock_guard<std::mutex> lk(mutex_);
    isr.set_term(current_term_);
    isr.set_id(id_);
    isr.set_need_snapshot(!should_install);
    isr.set_bytes_stored(bytes_stored);
    isr.set_last_included_index(last_included_index);
    isr.set_transfer_complete(should_install);
  }
  SendMessage(MessageType::InstallSnapshotResponseMsg, isr, leader_id);
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
  TermRelation tr;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    initial_role = role_;

    tr = TermCheckLocked(isr->term());
    if (tr == TermRelation::NEW) {
      demoted = DemoteSelfLocked(isr->term());
    }

    if (role_ != Role::LEADER || tr == TermRelation::STALE) {
      if (demoted) {
        leader_election_manager_->OnRoleChange();
      }
      return false;
    }

    if (!need_snapshot) {
      // Clear the in-progress record so future heartbeats can trigger a fresh
      // snapshot if needed.
      snapshot_send_time_[follower_id] =
          std::make_pair(std::chrono::steady_clock::time_point{}, size_t{0});
      if (transfer_complete) {
        LOG(INFO) << "ReceiveInstallSnapshotResponse: snapshot complete for "
                  << "follower " << follower_id
                  << " last_included_index=" << last_included_index;
        progress_[follower_id].state = ProgressState::REPLICATE;
        progress_[follower_id].next_index = last_included_index + 1;
        progress_[follower_id].match_index =
            std::max(progress_[follower_id].match_index, last_included_index);
        // If the follower still needs log entries, send them now.
        if (progress_[follower_id].next_index <= last_log_index_) {
          catchup_fields = GatherAeFieldsLocked(follower_id);
          send_catchup_ae = true;
        }
      } else {
        // The follower rejected the snapshot and does not need it.
        LOG(INFO) << "ReceiveInstallSnapshotResponse: Rejection from follower "
                  << follower_id;
        progress_[follower_id].state = ProgressState::PROBE;
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
    // EnqueueSnapshot checks ShouldSendSnapshotChunkLocked: bytes_stored is
    // a larger offset than what was last sent, so it will always be allowed
    // through immediately (progress condition).
    EnqueueSnapshot(follower_id, bytes_stored);
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
  for (size_t i = 0; i < progress_.size(); ++i) {
    oss << progress_[i].next_index;
    if (i + 1 != progress_.size()) {
      oss << ", ";
    }
  }
  oss << "]\n";

  oss << "match_index_: [";
  for (size_t i = 0; i < progress_.size(); ++i) {
    oss << progress_[i].match_index;
    if (i + 1 != progress_.size()) {
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
