#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chain/storage/memory_db.h"
#include "platform/consensus/checkpoint/mock_checkpoint.h"
#include "platform/consensus/ordering/raft/algorithm/raft_test_util.h"
#include "platform/consensus/recovery/raft_recovery.h"

namespace resdb {
namespace raft {

using resdb::raft::test_utils::CreateAeFields;
using resdb::raft::test_utils::CreateAeMessage;
using resdb::raft::test_utils::CreateLogEntries;
using resdb::raft::test_utils::GenerateConfig;
using resdb::raft::test_utils::MockBroadcastFunction;
using resdb::raft::test_utils::MockCommitFunction;
using resdb::raft::test_utils::MockSendMessageFunction;
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Invoke;

namespace {

InstallSnapshot MakeChunk(uint64_t term, int leaderId, uint64_t lastIndex,
                          uint64_t lastTerm, uint64_t offset,
                          const std::string& data, bool done) {
  InstallSnapshot msg;
  msg.set_term(term);
  msg.set_leaderid(leaderId);
  msg.set_lastincludedindex(lastIndex);
  msg.set_lastincludedterm(lastTerm);
  msg.set_offset(offset);
  msg.set_data(data);
  msg.set_done(done);
  return msg;
}

// Read all bytes from a file; returns empty string if the file doesn't exist.
std::string ReadFileContents(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  return {std::istreambuf_iterator<char>(f), {}};
}

// Check whether a path exists on disk.
bool FileExists(const std::string& path) {
  return std::filesystem::exists(path);
}

// Build a ResDBConfig with recovery enabled and a given WAL path.
ResDBConfig MakeConfig(const std::string& wal_path, int self_id = 1) {
  ResConfigData data;
  data.set_recovery_enabled(true);
  data.set_recovery_path(wal_path);
  data.set_recovery_buffer_size(1024);
  data.set_recovery_ckpt_time_s(3600);  // effectively disable background ckpt
  return ResDBConfig(
      {GenerateReplicaInfo(1, "127.0.0.1", 1234),
       GenerateReplicaInfo(2, "127.0.0.1", 1235),
       GenerateReplicaInfo(3, "127.0.0.1", 1236),
       GenerateReplicaInfo(4, "127.0.0.1", 1237)},
      GenerateReplicaInfo(self_id, "127.0.0.1", 1234 + self_id - 1), data);
}

// Drive the full leader to follower snapshot transfer loop in-process.
// Stops when the last response seen by the leader has transfer_complete=true,
// or after 1,000 iterations (which would indicate a bug).
void RunTransfer(Raft& leader, Raft& follower, int followerId, int leaderId,
                 MockSendMessageFunction& leader_send_message,
                 MockSendMessageFunction& follower_send_message) {
  std::vector<InstallSnapshot> toFollower;
  std::vector<InstallSnapshotResponse> toLeader;

  // To simulate sending messages, just push to them to a vector.
  EXPECT_CALL(leader_send_message, Call(_, _, _))
      .WillRepeatedly(::testing::Return(0));
  EXPECT_CALL(leader_send_message,
              Call(MessageType::InstallSnapshotMsg, _, followerId))
      .WillRepeatedly(
          Invoke([&](int, const google::protobuf::Message& msg, int) {
            LOG(INFO) << "Test sending install snapshot message";
            toFollower.push_back(dynamic_cast<const InstallSnapshot&>(msg));
            return 0;
          }));

  EXPECT_CALL(follower_send_message, Call(_, _, _))
      .WillRepeatedly(::testing::Return(0));
  EXPECT_CALL(follower_send_message,
              Call(MessageType::InstallSnapshotResponseMsg, _, leaderId))
      .WillRepeatedly(Invoke([&](int, const google::protobuf::Message& msg,
                                 int) {
        LOG(INFO) << "Test sending install snapshot response message";
        toLeader.push_back(dynamic_cast<const InstallSnapshotResponse&>(msg));
        return 0;
      }));

  // Send the first snapshot chunk
  leader.SendInstallSnapshot(followerId, /*byte_offset=*/0);
  EXPECT_EQ(toFollower.size(), 1);

  size_t fi = 0, li = 0;
  for (int iter = 0; iter < 1000; ++iter) {
    while (fi < toFollower.size()) {
      LOG(INFO) << "Follower is going to install snapshot";
      follower.ReceiveInstallSnapshot(
          std::make_unique<InstallSnapshot>(toFollower[fi++]));
    }
    while (li < toLeader.size()) {
      LOG(INFO) << "Leader is going to receive snapshot response";
      leader.ReceiveInstallSnapshotResponse(
          std::make_unique<InstallSnapshotResponse>(toLeader[li++]));
    }
    // Once the follower has responded to the leader with transfer_complete, the snapshot has completed.
    if (!toLeader.empty() && toLeader.back().transfer_complete()) {
      return;
    }
  }
  assert(false && "Snapshot sending went on for more than 1000 iterations");
}

// Mirrors what Consensus::RecoverFromLogs() does.
void RecoverFromLogs(RaftRecovery& recovery, Raft& raft) {
  recovery.ReadLogs(
      [](const RaftMetadata&) {},
      [&](std::unique_ptr<WALRecord> record) {
        switch (record->payload_case()) {
          case WALRecord::kEntry: {
            LogEntry le;
            le.entry = record->entry();
            raft.AddToLog(le, /*writeMetadata=*/false);
            break;
          }
          case WALRecord::kTruncation:
            raft.TruncateLog(record->truncation().truncate_from_index(),
                             /*writeMetadata=*/false);
            break;
          case WALRecord::PAYLOAD_NOT_SET:
            break;
        }
      },
      [&](const RaftMetadata& m) {
        raft.SetCurrentTerm(m.current_term, /*writeMetadata=*/false);
        raft.SetVotedFor(m.voted_for, /*writeMetadata=*/false);
        raft.SetSnapshotLastIndexAndTerm(m.snapshot_last_index,
                                         m.snapshot_last_term,
                                         /*writeMetadata=*/false);
      });
}

}  // namespace

class RaftSnapshotFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Each test gets its own log directory so they don't interfere.
    leader_log_ =
        "./log/snap_file_test_leader_" +
        std::to_string(
            ::testing::UnitTest::GetInstance()->current_test_info()->line()) +
        "/log";
    follower_log_ =
        "./log/snap_file_test_follower_" +
        std::to_string(
            ::testing::UnitTest::GetInstance()->current_test_info()->line()) +
        "/log";
    std::filesystem::remove_all(
        std::filesystem::path(leader_log_).parent_path());
    std::filesystem::remove_all(
        std::filesystem::path(follower_log_).parent_path());

    // Pre-create the directories so the single-level mkdir in RecoveryBase
    // succeeds.
    std::filesystem::create_directories(
        std::filesystem::path(leader_log_).parent_path());
    std::filesystem::create_directories(
        std::filesystem::path(follower_log_).parent_path());
  }

  std::string leader_log_;
  std::string follower_log_;
};

// Test 1 : After a completed single-chunk transfer the committed snapshot file
// exists on the follower's disk, and the temp file does not exist.
TEST_F(RaftSnapshotFileTest, SnapshotFileWrittenToFollowerDisk) {
  auto leader_storage = resdb::storage::NewMemoryDB();
  leader_storage->SetValueWithVersion("k1", "v1", 0);
  leader_storage->Flush();

  ResDBConfig leader_cfg = MakeConfig(leader_log_, /*self_id=*/1);
  RaftRecovery leader_recovery(leader_cfg, nullptr, leader_storage.get(),
                               nullptr);
  MockSignatureVerifier leader_verifier;
  MockLeaderElectionManager leader_leader_election_manager(leader_cfg);
  MockReplicaCommunicator leader_replica_communicator;
  MockSendMessageFunction leader_send_message;

  Raft leader(1, 1, 4, &leader_verifier, &leader_leader_election_manager,
              &leader_replica_communicator, &leader_recovery);
  leader.SetStateForTest({
      .currentTerm = 3,
      .role = Role::LEADER,
      .log = CreateLogEntries({}, true),
      .nextIndex = std::vector<uint64_t>{1, 1, 1, 1, 1},
      .matchIndex = std::vector<uint64_t>{0, 0, 0, 0, 0},
  });
  leader.SetSnapshotLastIndexAndTerm(2, 3, /*writeMetadata=*/false);
  leader.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return leader_send_message.Call(type, msg, node_id);
      });

  ResDBConfig follower_cfg = MakeConfig(follower_log_, /*self_id=*/2);
  auto follower_storage = resdb::storage::NewMemoryDB();
  RaftRecovery follower_recovery(follower_cfg, nullptr, follower_storage.get(),
                                 nullptr);
  MockSignatureVerifier follower_verifier;
  MockLeaderElectionManager follower_leader_election_manager(follower_cfg);
  MockReplicaCommunicator follower_replica_communicator;
  MockSendMessageFunction follower_send_message;
  MockCommitFunction follower_commit;

  Raft follower(2, 1, 4, &follower_verifier, &follower_leader_election_manager,
                &follower_replica_communicator, &follower_recovery);
  follower.SetStateForTest({.currentTerm = 3, .role = Role::FOLLOWER});
  follower.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return follower_send_message.Call(type, msg, node_id);
      });
  follower.SetCommitFunc([&](const google::protobuf::Message& msg) {
    return follower_commit.Commit(msg);
  });

  EXPECT_CALL(follower_leader_election_manager, OnHeartBeat())
      .Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());
  EXPECT_CALL(follower_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());
  ASSERT_EQ(leader_recovery.GetStorage(), leader_storage.get())
      << "RaftRecovery::GetStorage() must return the same Storage passed in";
  auto items = leader_storage->GetAllItems();
  ASSERT_FALSE(items.empty()) << "leader_storage has no items before transfer";

  RunTransfer(leader, follower, 2, 1, leader_send_message,
              follower_send_message);

  // The committed snapshot file must exist on the follower's disk.
  const std::string snap_path = follower.GetSnapshotFilePath();
  EXPECT_TRUE(FileExists(snap_path))
      << "Expected snapshot file at " << snap_path;

  // The temp file must have been cleaned up (renamed away).
  auto temp_path = follower.GetSnapshotFilePath() + ".recv.tmp";
  EXPECT_FALSE(FileExists(temp_path))
      << "Temp file should not exist after completed transfer: " << temp_path;

  // Data must round-trip correctly.
  EXPECT_EQ(follower_storage->GetValue("k1"), "v1");
}

// Test 2: A follower that receives only some chunks starts fresh on the next
// transfer. It does not get stuck because the old temp file is replaced, not
// appended to.
TEST_F(RaftSnapshotFileTest, PartialTransferDoesNotCorruptSubsequentTransfer) {
  auto leader_storage = resdb::storage::NewMemoryDB();
  leader_storage->SetValueWithVersion("real_key", "real_value", 0);

  ResDBConfig leader_cfg = MakeConfig(leader_log_, 1);
  RaftRecovery leader_recovery(leader_cfg, nullptr, leader_storage.get(),
                               nullptr);
  MockSignatureVerifier leader_verifier;
  MockLeaderElectionManager leader_leader_election_manager(leader_cfg);
  MockReplicaCommunicator leader_replica_communicator;

  Raft leader(1, 1, 4, &leader_verifier, &leader_leader_election_manager,
              &leader_replica_communicator, &leader_recovery);
  leader.SetStateForTest({
      .currentTerm = 4,
      .role = Role::LEADER,
      .log = CreateLogEntries({}, true),
      .nextIndex = std::vector<uint64_t>{1, 1, 1, 1, 1},
      .matchIndex = std::vector<uint64_t>{0, 0, 0, 0, 0},
  });
  leader.SetSnapshotLastIndexAndTerm(5, 4, /*writeMetadata=*/false);

  ResDBConfig follower_cfg = MakeConfig(follower_log_, 2);
  auto follower_storage = resdb::storage::NewMemoryDB();
  RaftRecovery follower_recovery(follower_cfg, nullptr, follower_storage.get(),
                                 nullptr);
  MockSignatureVerifier follower_verifier;
  MockLeaderElectionManager follower_leader_election_manager(follower_cfg);
  MockReplicaCommunicator follower_replica_communicator;
  MockSendMessageFunction follower_send_message;
  MockCommitFunction follower_commit;

  Raft follower(2, 1, 4, &follower_verifier, &follower_leader_election_manager,
                &follower_replica_communicator, &follower_recovery);
  follower.SetStateForTest({.currentTerm = 4, .role = Role::FOLLOWER});
  follower.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return follower_send_message.Call(type, msg, node_id);
      });
  follower.SetCommitFunc([&](const google::protobuf::Message& msg) {
    return follower_commit.Commit(msg);
  });

  EXPECT_CALL(follower_leader_election_manager, OnHeartBeat())
      .Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());
  EXPECT_CALL(follower_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());

  // Simulate a partial transfer: send only the first chunk (done == false) and
  // do not deliver any follow-up. The follower will have an open temp file
  // in pending state.
  {
    std::vector<InstallSnapshotResponse> responses;
    EXPECT_CALL(follower_send_message, Call(_, _, _))
        .WillRepeatedly(::testing::Return(0));
    EXPECT_CALL(follower_send_message,
                Call(MessageType::InstallSnapshotResponseMsg, _, _))
        .WillRepeatedly(
            Invoke([&](int, const google::protobuf::Message& msg, int) {
              responses.push_back(
                  dynamic_cast<const InstallSnapshotResponse&>(msg));
              return 0;
            }));

    // Inject a fake first chunk with done == false. Since the real leader
    // serializes to a file, we use a raw chunk to avoid needing the leader's
    // file path.
    auto partial = MakeChunk(4, /*leaderId=*/1, /*lastIndex=*/5,
                             /*lastTerm=*/4, /*offset=*/0,
                             std::string(256, 'P'), /*done=*/false);
    follower.ReceiveInstallSnapshot(
        std::make_unique<InstallSnapshot>(std::move(partial)));

    ASSERT_FALSE(responses.empty());
    EXPECT_TRUE(responses[0].need_snapshot());
    EXPECT_EQ(responses[0].bytes_stored(), 256u);
    // Temp file should exist now.
    auto temp_path = follower.GetSnapshotFilePath() + ".recv.tmp";
    EXPECT_TRUE(FileExists(temp_path)) << "Path being checked: " << temp_path;
  }

  // Now run a complete, fresh transfer from the beginning. The fresh offset ==
  // 0 chunk must discard the previous partial state and open a new temp file.
  MockSendMessageFunction leader_send_message2;
  leader.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return leader_send_message2.Call(type, msg, node_id);
      });

  RunTransfer(leader, follower, 2, 1, leader_send_message2,
              follower_send_message);

  // After completion the committed file must exist.
  EXPECT_TRUE(FileExists(follower.GetSnapshotFilePath()));
  // Temp file must be gone.
  auto temp_path = follower.GetSnapshotFilePath() + ".recv.tmp";
  EXPECT_FALSE(FileExists(temp_path));
  EXPECT_EQ(follower_storage->GetValue("real_key"), "real_value");
}

// Test 3: When the leader's snapshot_last_index advances between two transfers,
// the leader writes a new snapshot file at offset == 0 that reflects the
// updated state, not the stale one.
TEST_F(RaftSnapshotFileTest, LeaderReserializesWhenSnapshotIndexAdvances) {
  auto leader_storage = resdb::storage::NewMemoryDB();
  leader_storage->SetValueWithVersion("v1_key", "v1_val", 0);

  ResDBConfig leader_cfg = MakeConfig(leader_log_, 1);
  RaftRecovery leader_recovery(leader_cfg, nullptr, leader_storage.get(),
                               nullptr);
  MockSignatureVerifier leader_verifier;
  MockLeaderElectionManager leader_leader_election_manager(leader_cfg);
  MockReplicaCommunicator leader_replica_communicator;
  MockSendMessageFunction leader_send_message;

  Raft leader(1, 1, 4, &leader_verifier, &leader_leader_election_manager,
              &leader_replica_communicator, &leader_recovery);
  leader.SetStateForTest({
      .currentTerm = 2,
      .role = Role::LEADER,
      .log = CreateLogEntries({}, true),
      .nextIndex = std::vector<uint64_t>{1, 1, 1, 1, 1},
      .matchIndex = std::vector<uint64_t>{0, 0, 0, 0, 0},
  });
  leader.SetSnapshotLastIndexAndTerm(3, 2, /*writeMetadata=*/false);
  leader.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return leader_send_message.Call(type, msg, node_id);
      });
  // Transfer the first chunk of snapshot to follower A
  const std::string follower_a_log = follower_log_ + "_a";
  std::filesystem::remove_all(
      std::filesystem::path(follower_a_log).parent_path());
  std::filesystem::create_directories(
      std::filesystem::path(follower_a_log).parent_path());
  ResDBConfig follower_a_cfg = MakeConfig(follower_a_log, 2);
  auto follower_a_storage = resdb::storage::NewMemoryDB();
  RaftRecovery follower_a_recovery(follower_a_cfg, nullptr,
                                   follower_a_storage.get(), nullptr);
  MockSignatureVerifier follower_a_verifier;
  MockLeaderElectionManager follower_a_leader_election_manager(follower_a_cfg);
  MockReplicaCommunicator follower_a_replica_communicator;
  MockSendMessageFunction follower_a_send_message;
  MockCommitFunction follower_a_commit;

  Raft follower_a(2, 1, 4, &follower_a_verifier,
                  &follower_a_leader_election_manager,
                  &follower_a_replica_communicator, &follower_a_recovery);
  follower_a.SetStateForTest({.currentTerm = 2, .role = Role::FOLLOWER});
  follower_a.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return follower_a_send_message.Call(type, msg, node_id);
      });
  follower_a.SetCommitFunc([&](const google::protobuf::Message& msg) {
    return follower_a_commit.Commit(msg);
  });

  EXPECT_CALL(follower_a_leader_election_manager, OnHeartBeat())
      .Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
  EXPECT_CALL(leader_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());
  EXPECT_CALL(follower_a_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());

  RunTransfer(leader, follower_a, 2, 1, leader_send_message,
              follower_a_send_message);
  EXPECT_EQ(follower_a_storage->GetValue("v1_key"), "v1_val");

  // Capture the file contents written during the first transfer.
  const std::string leader_snap_path = leader.GetSnapshotFilePath();
  std::string first_snapshot_bytes = ReadFileContents(leader_snap_path);
  ASSERT_FALSE(first_snapshot_bytes.empty());

  // Advance the leader's snapshot point and update storage.
  leader_storage->SetValueWithVersion("v2_key", "v2_val", 0);
  leader.SetSnapshotLastIndexAndTerm(7, 2, /*writeMetadata=*/false);

  // After a new snapshot has been taken, send the first snapshot chunk to
  // follower B.
  const std::string follower_b_log = follower_log_ + "_b";
  std::filesystem::remove_all(
      std::filesystem::path(follower_b_log).parent_path());
  std::filesystem::create_directories(
      std::filesystem::path(follower_b_log).parent_path());
  ResDBConfig follower_b_cfg = MakeConfig(follower_b_log, 3);
  auto follower_b_storage = resdb::storage::NewMemoryDB();
  RaftRecovery follower_b_recovery(follower_b_cfg, nullptr,
                                   follower_b_storage.get(), nullptr);
  MockSignatureVerifier follower_b_verifier;
  MockLeaderElectionManager follower_b_leader_election_manager(follower_b_cfg);
  MockReplicaCommunicator follower_b_replica_communicator;
  MockSendMessageFunction follower_b_send_message;
  MockCommitFunction follower_b_commit;

  Raft follower_b(3, 1, 4, &follower_b_verifier,
                  &follower_b_leader_election_manager,
                  &follower_b_replica_communicator, &follower_b_recovery);
  follower_b.SetStateForTest({.currentTerm = 2, .role = Role::FOLLOWER});
  follower_b.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return follower_b_send_message.Call(type, msg, node_id);
      });
  follower_b.SetCommitFunc([&](const google::protobuf::Message& msg) {
    return follower_b_commit.Commit(msg);
  });

  EXPECT_CALL(follower_b_leader_election_manager, OnHeartBeat())
      .Times(AnyNumber());
  EXPECT_CALL(follower_b_leader_election_manager, OnRoleChange())
      .Times(AnyNumber());

  MockSendMessageFunction leader_send_message2;
  leader.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return leader_send_message2.Call(type, msg, node_id);
      });

  RunTransfer(leader, follower_b, 3, 1, leader_send_message2,
              follower_b_send_message);

  // The snapshot file on disk must have been updated (different bytes).
  std::string second_snapshot_bytes = ReadFileContents(leader_snap_path);
  EXPECT_NE(first_snapshot_bytes, second_snapshot_bytes)
      << "Leader should have written a new snapshot file after advancing its "
         "snapshot_last_index";

  // follower_b must have both keys; follower_a must only have v1_key.
  EXPECT_EQ(follower_b_storage->GetValue("v1_key"), "v1_val");
  EXPECT_EQ(follower_b_storage->GetValue("v2_key"), "v2_val");
  EXPECT_EQ(follower_a_storage->GetValue("v2_key"), "");
}

// Test 4: A follower installs a snapshot, then restarts, reading the metadata
// and snapshot.
TEST_F(RaftSnapshotFileTest, FollowerMetadataSurvivesRestartAfterSnapshot) {
  auto leader_storage = resdb::storage::NewMemoryDB();
  leader_storage->SetValueWithVersion("persist_key", "persist_value", 0);
  leader_storage->SetValueWithVersion("another_key", "another_value", 0);

  ResDBConfig leader_cfg = MakeConfig(leader_log_, 1);
  RaftRecovery leader_recovery(leader_cfg, nullptr, leader_storage.get(),
                               nullptr);
  MockSignatureVerifier leader_verifier;
  MockLeaderElectionManager leader_leader_election_manager(leader_cfg);
  MockReplicaCommunicator leader_replica_communicator;
  MockSendMessageFunction leader_send_message;

  Raft leader(1, 1, 4, &leader_verifier, &leader_leader_election_manager, &leader_replica_communicator, &leader_recovery);
  leader.SetStateForTest({
      .currentTerm = 6,
      .role = Role::LEADER,
      .log = CreateLogEntries({}, true),
      .nextIndex = std::vector<uint64_t>{1, 1, 1, 1, 1},
      .matchIndex = std::vector<uint64_t>{0, 0, 0, 0, 0},
  });
  leader.SetSnapshotLastIndexAndTerm(10, 6, /*writeMetadata=*/false);
  leader.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return leader_send_message.Call(type, msg, node_id);
      });

  ResDBConfig follower_cfg = MakeConfig(follower_log_, 2);
  // Use a real MemoryDB; it is the same object across both lifetimes so we
  // can observe that Clear() + SetValue() happened (MemoryDB keeps state in
  // memory, but WriteMetadata() persists snapshot_last_index to disk).
  auto follower_storage = resdb::storage::NewMemoryDB();
  follower_storage->SetValueWithVersion("stale_data", "will_be_cleared", 0);

  {
    RaftRecovery follower_recovery(follower_cfg, nullptr,
                                   follower_storage.get(), nullptr);
    MockSignatureVerifier follower_verifier;
    MockLeaderElectionManager follower_leader_election_manager(follower_cfg);
    MockReplicaCommunicator follower_replica_communicator;
    MockSendMessageFunction follower_send_message;
    MockCommitFunction follower_commit;

    Raft follower(2, 1, 4, &follower_verifier, &follower_leader_election_manager, &follower_replica_communicator, &follower_recovery);
    follower.SetStateForTest({.currentTerm = 6, .role = Role::FOLLOWER});
    follower.SetSingleCallFunc(
        [&](int type, const google::protobuf::Message& msg, int node_id) {
          return follower_send_message.Call(type, msg, node_id);
        });
    follower.SetCommitFunc([&](const google::protobuf::Message& msg) {
      return follower_commit.Commit(msg);
    });

    EXPECT_CALL(follower_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
    EXPECT_CALL(leader_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
    EXPECT_CALL(leader_leader_election_manager, OnRoleChange()).Times(AnyNumber());
    EXPECT_CALL(follower_leader_election_manager, OnRoleChange()).Times(AnyNumber());

    RunTransfer(leader, follower, 2, 1, leader_send_message,
                follower_send_message);

    // Verify storage is correct while follower is still alive.
    EXPECT_EQ(follower_storage->GetValue("persist_key"), "persist_value");
    EXPECT_EQ(follower_storage->GetValue("stale_data"), "");
    EXPECT_EQ(follower.GetSnapshotLastIndex(), 10u);
  }

  {
    RaftRecovery recovery2(follower_cfg, nullptr, follower_storage.get(),
                           nullptr);
    MockSignatureVerifier fv2;
    MockLeaderElectionManager fl2(follower_cfg);
    MockReplicaCommunicator fc2;

    Raft follower2(2, 1, 4, &fv2, &fl2, &fc2, &recovery2);
    RecoverFromLogs(recovery2, follower2);

    EXPECT_EQ(follower2.GetSnapshotLastIndex(), 10u)
        << "snapshot_last_index must survive process restart";
    EXPECT_EQ(follower2.GetCurrentTerm(), 6u)
        << "currentTerm must survive process restart";

    // The storage data is still in-memory (same object), but this verifies the
    // stale key is gone.
    EXPECT_EQ(follower_storage->GetValue("persist_key"), "persist_value");
    EXPECT_EQ(follower_storage->GetValue("another_key"), "another_value");
    EXPECT_EQ(follower_storage->GetValue("stale_data"), "");
  }
}

// Test 5: A follower receives the first chunk of a different snapshot while a
// prior temp file is open correctly discards the old temp file and starts
// fresh.
TEST_F(RaftSnapshotFileTest, FollowerDiscardsOldTempFileOnNewOffsetZeroChunk) {
  // Use real RaftRecovery so snapshot file paths are real.
  ResDBConfig follower_cfg = MakeConfig(follower_log_, 2);
  auto follower_storage = resdb::storage::NewMemoryDB();
  RaftRecovery follower_recovery(follower_cfg, nullptr, follower_storage.get(),
                                 nullptr);
  MockSignatureVerifier follower_verifier;
  MockLeaderElectionManager follower_leader_election_manager(follower_cfg);
  MockReplicaCommunicator follower_replica_communicator;
  MockSendMessageFunction follower_send_message;
  MockCommitFunction follower_commit;

  Raft follower(2, 1, 4, &follower_verifier, &follower_leader_election_manager, &follower_replica_communicator, &follower_recovery);
  follower.SetStateForTest({.currentTerm = 5, .role = Role::FOLLOWER});
  follower.SetSingleCallFunc(
      [&](int type, const google::protobuf::Message& msg, int node_id) {
        return follower_send_message.Call(type, msg, node_id);
      });
  follower.SetCommitFunc([&](const google::protobuf::Message& msg) {
    return follower_commit.Commit(msg);
  });

  EXPECT_CALL(follower_leader_election_manager, OnHeartBeat()).Times(AnyNumber());
  EXPECT_CALL(follower_leader_election_manager, OnRoleChange()).Times(AnyNumber());

  std::vector<InstallSnapshotResponse> responses;
  EXPECT_CALL(follower_send_message, Call(_, _, _))
      .WillRepeatedly(::testing::Return(0));
  EXPECT_CALL(follower_send_message,
              Call(MessageType::InstallSnapshotResponseMsg, _, _))
      .WillRepeatedly(Invoke([&](int, const google::protobuf::Message& msg,
                                 int) {
        responses.push_back(dynamic_cast<const InstallSnapshotResponse&>(msg));
        return 0;
      }));

  // First attempt: send offset == 0 chunk only (partial).
  const std::string first_data(200, 'A');
  auto c1 = MakeChunk(5, 1, 15, 5, 0, first_data, /*done=*/false);
  follower.ReceiveInstallSnapshot(
      std::make_unique<InstallSnapshot>(std::move(c1)));
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(responses[0].bytes_stored(), 200u);

  // Simulate leader restarting the transfer. The follower must accept it and
  // discard the old partial snapshot.
  const std::string second_data(100, 'B');
  auto c2 = MakeChunk(5, 1, 15, 5, 0, second_data, /*done=*/true);
  follower.ReceiveInstallSnapshot(
      std::make_unique<InstallSnapshot>(std::move(c2)));

  ASSERT_EQ(responses.size(), 2u);
  EXPECT_TRUE(responses[1].transfer_complete());
  EXPECT_FALSE(responses[1].need_snapshot());

  // Snapshot index must have advanced.
  EXPECT_EQ(follower.GetSnapshotLastIndex(), 15u);

  // The committed snapshot file on disk should contain exactly 'B'*100.
  std::string on_disk = ReadFileContents(follower.GetSnapshotFilePath());
  EXPECT_EQ(on_disk.size(), 100u);
  EXPECT_TRUE(on_disk.find_first_not_of('B') == std::string::npos)
      << "Committed snapshot file should contain only the second attempt's "
         "data";
}

}  // namespace raft
}  // namespace resdb
