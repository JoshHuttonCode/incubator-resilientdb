#pragma once

#include <gmock/gmock.h>

#include "common/crypto/mock_signature_verifier.h"
#include "platform/config/resdb_config_utils.h"
#include "platform/consensus/ordering/raft/algorithm/mock_leader_election_manager.h"
#include "platform/consensus/ordering/raft/algorithm/raft.h"
#include "platform/networkstrate/mock_replica_communicator.h"
#include "platform/proto/client_test.pb.h"

namespace resdb {
namespace raft {
namespace test_utils {

inline ResDBConfig GenerateConfig() {
  ResConfigData data;
  data.set_duplicate_check_frequency_useconds(100000);
  data.set_enable_viewchange(true);
  return ResDBConfig({GenerateReplicaInfo(1, "127.0.0.1", 1234),
                      GenerateReplicaInfo(2, "127.0.0.1", 1235),
                      GenerateReplicaInfo(3, "127.0.0.1", 1236),
                      GenerateReplicaInfo(4, "127.0.0.1", 1237)},
                     GenerateReplicaInfo(1, "127.0.0.1", 1234), data);
}

class MockSendMessageFunction {
 public:
  MOCK_METHOD(int, Call, (int, const google::protobuf::Message&, int));
};
class MockBroadcastFunction {
 public:
  MOCK_METHOD(int, Broadcast, (int, const google::protobuf::Message&));
};
class MockCommitFunction {
 public:
  MOCK_METHOD(int, Commit, (const google::protobuf::Message&));
};

inline AeFields CreateAeFields(uint64_t term, int leader_id,
                               uint64_t prev_log_index, uint64_t prev_log_term,
                               const std::vector<LogEntry>& entries,
                               uint64_t leader_commit, int follower_id) {
  AeFields fields{};
  fields.term = term;
  fields.leader_id = leader_id;
  fields.leader_commit = leader_commit;
  fields.prev_log_index = prev_log_index;
  fields.prev_log_term = prev_log_term;
  fields.follower_id = follower_id;

  for (const auto& entry : entries) {
    LogEntry log_entry;
    log_entry.entry.set_term(entry.entry.term());
    log_entry.entry.set_command(entry.entry.command());
    fields.entries.push_back(std::move(log_entry));
  }

  return fields;
};

// Helper to create a single log entry.
inline LogEntry CreateLogEntry(uint64_t term, const std::string& command_data) {
  LogEntry log_entry;
  log_entry.entry.set_term(term);
  log_entry.entry.set_command(command_data);
  return log_entry;
}

// Helper to create a vector of log entries for testing.
inline std::vector<LogEntry> CreateLogEntries(
    const std::vector<std::pair<uint64_t, std::string>>& term_and_cmds,
    bool used_for_log_patch = false) {
  std::vector<LogEntry> entries;

  if (used_for_log_patch) {
    LogEntry first_entry;
    first_entry.entry.set_term(0);
    first_entry.entry.set_command("COMMON_PREFIX");
    entries.push_back(first_entry);
  }

  for (const auto& [term, cmd] : term_and_cmds) {
    LogEntry log_entry;
    log_entry.entry.set_term(term);

    ClientTestRequest req;
    req.set_value(cmd);

    std::string serialized;
    req.SerializeToString(&serialized);
    log_entry.entry.set_command(serialized);

    entries.push_back(log_entry);
  }

  return entries;
}

inline AppendEntries CreateAeMessage(const AeFields& fields) {
  AppendEntries ae;
  ae.set_term(fields.term);
  ae.set_leader_id(fields.leader_id);
  ae.set_prev_log_index(fields.prev_log_index);
  ae.set_prev_log_term(fields.prev_log_term);
  ae.set_leader_commit_index(fields.leader_commit);
  for (const auto& log_entry : fields.entries) {
    auto* new_entry = ae.add_entries();
    new_entry->set_term(log_entry.entry.term());
    new_entry->set_command(log_entry.entry.command());
  }

  return ae;
}

}  // namespace test_utils
}  // namespace raft
}  // namespace resdb
