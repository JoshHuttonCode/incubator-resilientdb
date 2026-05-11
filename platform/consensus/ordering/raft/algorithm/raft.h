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

#pragma once

#include <sys/types.h>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <thread>
#include <chrono>
#ifdef RAFT_TEST_MODE
#include <ostream>
#endif

#include "platform/common/queue/lock_free_queue.h"
#include "platform/consensus/ordering/common/algorithm/protocol_base.h"
#include "platform/consensus/ordering/raft/algorithm/leader_election_manager.h"
#include "platform/consensus/ordering/raft/proto/proposal.pb.h"
#include "platform/consensus/recovery/raft_recovery.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/proto/resdb.pb.h"
#include "platform/statistic/stats.h"

namespace resdb {
namespace raft {

enum class Role { FOLLOWER, CANDIDATE, LEADER };
enum class TermRelation { STALE, CURRENT, NEW };

class LogEntry {
  public:
  Entry entry;

  uint32_t GetSerializedSize() const;
  uint32_t ComputeSerializedEntrySize() const;
  
  private:
   mutable uint32_t serialized_size = 0;
};

struct AeFields {
  uint64_t term = 0;
  int leader_id = -1;
  uint64_t prev_log_index = 0;
  uint64_t prev_log_term = 0;
  std::vector<LogEntry> entries{};
  uint64_t leader_commit = 0;
  int follower_id =
      -1;  // not part of AE message itself, but needed to determine recipient
};

struct InFlightMsg {
  std::chrono::steady_clock::time_point time_sent;
  uint64_t prev_log_index_sent;
  uint64_t last_index_of_segment_sent;
};

#ifdef RAFT_TEST_MODE
struct RaftStatePatch {
  std::optional<uint64_t> current_term;
  std::optional<int> voted_for;
  std::optional<uint64_t> commit_index;
  std::optional<uint64_t> last_committed;
  std::optional<Role> role;

  std::optional<std::vector<LogEntry>> log;
  std::optional<std::vector<uint64_t>> next_index;
  std::optional<std::vector<uint64_t>> match_index;
  std::optional<std::vector<int>> votes;
  std::optional<std::vector<std::vector<InFlightMsg>>> in_flight_vecs;
};
#endif

class Raft : public common::ProtocolBase {
 public:
  Raft(int id, int f, int total_num, SignatureVerifier* verifier,
       LeaderElectionManager* leader_election_manager,
       ReplicaCommunicator* replica_communicator, RaftRecovery* recovery);
  ~Raft();

  const bool replication_logging_flag_ = true;
  const bool liveness_logging_flag_ = false;

  virtual bool ReceiveTransaction(std::unique_ptr<Request> req);
  virtual bool ReceiveAppendEntries(std::unique_ptr<AppendEntries> ae);
  virtual bool ReceiveAppendEntriesResponse(
      std::unique_ptr<AppendEntriesResponse> aer);
  virtual void ReceiveRequestVote(std::unique_ptr<RequestVote> rv);
  virtual void ReceiveRequestVoteResponse(
      std::unique_ptr<RequestVoteResponse> rvr);
  virtual bool ReceiveInstallSnapshot(std::unique_ptr<InstallSnapshot> is);
  virtual bool ReceiveInstallSnapshotResponse(
      std::unique_ptr<InstallSnapshotResponse> isr);
  virtual void StartElection();
  virtual void SendHeartBeat();
  virtual Role GetRoleSnapshot() const;
  virtual void SetRole(Role role);
  virtual void PrintDebugStateLocked() const;
  virtual void PrintDebugState() const;
  void WriteMetadata();
  uint64_t GetSnapshotLastIndex();

  // These functions with write_metadata are also used to replay information
  // upon recovery. So, they are called with false during recovery, and true
  // everywhere else.
  virtual void SetCurrentTerm(uint64_t current_term,
                              bool write_metadata = true);
  virtual void SetVotedFor(int votedFor, bool write_metadata = true);
  virtual void SetCurrentTermAndVotedFor(uint64_t current_term, int voted_for,
                                         bool write_metadata = true);
  void SetSnapshotLastIndexAndTerm(uint64_t snapshot_last_index,
                                   uint64_t snapshot_last_term,
                                   bool write_metadata = true);
  void AddToLog(LogEntry& log_entry, bool write_metadata = true);
  void AddToLog(std::vector<LogEntry> logEntriesToAdd,
                bool write_metadata = true);
  void TruncateLog(uint64_t first, bool write_metadata = true);
  void TruncatePrefix(uint64_t index);
  bool IsSendSnapshotInProgress() {
    for (auto in_progress : snapshot_in_progress_) {
      if (in_progress) {
        return true;
      }
    }
    return false;
  };

 private:
  mutable std::mutex mutex_;

  virtual TermRelation TermCheckLocked(
      uint64_t term) const;                       // Must be called under mutex
  virtual bool DemoteSelfLocked(uint64_t term);   // Must be called under mutex
  virtual uint64_t GetLastLogTermLocked() const;  // Must be called under mutex
  virtual bool IsStop();
  //bool IsDuplicateLogEntry(const std::string& hash) const; // Must be called under mutex
  virtual std::vector<std::unique_ptr<Request>>
  PrepareCommitLocked();  // Must be called under mutex
  virtual AeFields GatherAeFieldsLocked(int follower_id, bool heartBeat = false)
      const;  // Must be called under mutex
  std::vector<AeFields> GatherAeFieldsForBroadcastLocked(bool heartBeat = false) const; // Must be called under mutex
  virtual void CreateAndSendAppendEntryMsg(const AeFields& fields);
  virtual LogEntry CreateLogEntry(const Entry& entry) const;
  virtual void ClearInFlightsLocked();
  virtual void PruneExpiredInFlightMsgsLocked();
  virtual void PruneRedundantInFlightMsgsLocked(
      int follower_id, uint64_t followerlast_log_index);
  virtual void RecordNewInFlightMsgLocked(
      const AeFields& msg, std::chrono::steady_clock::time_point timestamp);

#ifdef RAFT_TEST_MODE
 public:
  std::string GetSnapshotFilePath() const { return snapshot_file_path_; }
#endif
  virtual bool InFlightPerFollowerLimitReachedLocked(int follower_id) const;
  int GetLogicalLogSize() const;
  const LogEntry& GetLogEntryAtIndex(uint64_t index) const;
  const uint64_t GetLogTermAtIndex(uint64_t index) const;
  void SendInstallSnapshot(int follower_id, size_t byte_offset);
#ifdef RAFT_TEST_MODE
 private:
#endif
  void TruncatePrefixLocked(uint64_t index);
  void SetRoleLocked(Role role);

  // Persistent state on all servers:
  uint64_t current_term_;     // Protected by mutex_
  int voted_for_;             // Protected by mutex_
  std::vector<LogEntry> log_; // Protected by mutex_

  // Volatile state on leaders:
  std::vector<uint64_t> next_index_;    // Protected by mutex_
  std::vector<uint64_t> match_index_;   // Protected by mutex_
  uint64_t heartbeats_sent_this_term_;  // Protected by mutex_
  uint64_t last_log_index_;             // Protected by mutex_

  // Volatile state on all servers:
  uint64_t commit_index_;  // Protected by mutex_
  // last_committed stores the last entry that has been passed to commit_, but
  // it may not yet have been executed. Raft's Consensus file holds lastApplied_
  uint64_t last_committed_;  // Protected by mutex_
  Role role_; // Protected by mutex_
  // int leader_id_; // Protected by mutex_
  std::vector<int> votes_; // Protected by mutex_
  std::vector<std::vector<InFlightMsg>> in_flight_vecs_;  // Protected by mutex_
  //std::chrono::steady_clock::time_point last_ae_time_;
  //std::chrono::steady_clock::time_point last_heartbeat_time_; // Protected by mutex_
  int64_t snapshot_last_index_;
  int64_t snapshot_last_term_;
  std::vector<bool> snapshot_in_progress_;  // Protected by mutex_

  // Reassembly state for incoming chunked InstallSnapshot RPCs (follower side).
  // Key: leader_id. Cleared when snapshot finishes or a new one starts.
  // Chunks are written to a temp file as they arrive. On completion the temp
  // file is renamed to the final snapshot path and applied to the state
  // machine.
  struct PendingSnapshot {
    uint64_t last_included_index = 0;
    uint64_t last_included_term = 0;
    // next byte offset we expect from the leader
    uint64_t expected_offset = 0;
    // open fd to the temp file, or -1 if not open
    int fd = -1;
    std::string tmp_path;
  };
  std::map<int, PendingSnapshot>
      pending_snapshot_chunks_;  // Protected by mutex_
  // 1 MiB per snapshot chunk
  static constexpr size_t chunk_size_in_bytes_ = 1 * 1024 * 1024;

  // final committed snapshot path
  std::string snapshot_file_path_;
  // temp path used during leader serialization
  std::string snapshot_tmp_path_;

  bool is_stop_;
  const uint64_t quorum_;

  // for limiting AppendEntries batch sizing
  static constexpr size_t max_header_bytes_ = 64;
  static constexpr size_t max_bytes_ = 64 * 1024;
  static constexpr size_t max_entries_ = 16;
  static constexpr size_t max_in_flight_per_follower_ = 4;
  static constexpr std::chrono::milliseconds ae_response_deadline_{
      300};  // in milliseconds

  SignatureVerifier* verifier_;
  LeaderElectionManager* leader_election_manager_;
  //Stats* global_stats_;
  ReplicaCommunicator* replica_communicator_;
  RaftRecovery* recovery_;

#ifdef RAFT_TEST_MODE
 public:
  void SetStateForTest(RaftStatePatch patch) {
    std::lock_guard lk(mutex_);
    if (patch.current_term) current_term_ = *patch.current_term;
    if (patch.voted_for) voted_for_ = *patch.voted_for;
    if (patch.commit_index) commit_index_ = *patch.commit_index;
    if (patch.last_committed) last_committed_ = *patch.last_committed;
    if (patch.role) role_ = *patch.role;

    if (patch.log) {
      log_ = *patch.log;
      last_log_index_ = log_.size() - 1 + snapshot_last_index_;
    }

    if (patch.next_index) next_index_ = *patch.next_index;
    if (patch.match_index) match_index_ = *patch.match_index;
    if (patch.votes) votes_ = *patch.votes;
    if (patch.in_flight_vecs) in_flight_vecs_ = *patch.in_flight_vecs;
  }

  uint64_t GetCurrentTerm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_term_;
  }

  int GetVotedFor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voted_for_;
  }

  const std::vector<LogEntry>& GetLog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_;
  }

  void PrintLog(std::ostream& os) const {
    os << "Log entries (count = " << log_.size() << "):\n";

    for (size_t i = 0; i < log_.size(); ++i) {
      const auto& entry = log_[i];

      os << "  [" << i << "] "
         << "term=" << entry.entry.term() << ", command=\""
         << entry.entry.command() << "\""
         << ", serialized_size=" << entry.GetSerializedSize() << "\n";
    }
  }

  size_t GetLogSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_.size();
  }

  uint64_t GetLastLogIndexFromLog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_.empty() ? 0 : log_.size() - 1;
  }

  std::vector<uint64_t> GetNextIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_index_;
  }

  std::vector<uint64_t> GetMatchIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return match_index_;
  }

  uint64_t GetHeartBeatsSentThisTerm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heartbeats_sent_this_term_;
  }

  uint64_t GetLastLogIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_log_index_;
  }

  uint64_t GetCommitIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_index_;
  }

  uint64_t GetLastCommitted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_committed_;
  }

  Role GetRole() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
  }

  std::vector<int> GetVotes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return votes_;
  }

  std::vector<std::vector<InFlightMsg>> GetInFlightVecs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_vecs_;
  }

  size_t GetMaxInFlightVecs() const {
    return max_in_flight_per_follower_;
  }

#endif
};

}  // namespace raft
}  // namespace resdb
