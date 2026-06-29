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

#include "platform/consensus/ordering/raft/algorithm/raft_tests.h"

namespace resdb {
namespace raft {

// Test 1: A follower receiving a client transaction should reject it.
TEST_F(RaftTest, FollowerRejectsClientTransaction) {
  EXPECT_CALL(mock_call, Call(MessageType::AppendEntriesResponseMsg, _, _))
      .WillOnce(Invoke(
          [&](int, const google::protobuf::Message& msg, int) { return 0; }));
  EXPECT_CALL(mock_call, Call(MessageType::DirectToLeaderMsg, _, _))
      .WillOnce(Invoke([&](int, const google::protobuf::Message& msg, int) {
        const auto& dtl = dynamic_cast<const DirectToLeader&>(msg);
        EXPECT_EQ(dtl.leader_id(), 2);
        return 0;
      }));
  EXPECT_CALL(*leader_election_manager_, OnHeartBeat()).Times(1);
  EXPECT_CALL(mock_broadcast, Broadcast(_, _)).Times(0);

  raft_->SetStateForTest({
      .current_term = 0,
      .role = Role::FOLLOWER,
      .log = CreateLogEntries({}, true),
  });

  // Set up the client so it can send the DirectToLeader message.
  ReplicaInfo client;
  client.set_ip("127.0.0.1");
  client.set_port(1236);
  client.set_id(3);
  replica_communicator_->UpdateClientReplicas({client});

  // Receive an AppendEntries heartbeat to set current_leader_.
  auto ae_fields = CreateAeFields(
      /*term=*/0,
      /*leader_id=*/2,
      /*prev_log_index=*/0,
      /*prev_log_term=*/0,
      /*entries=*/
      CreateLogEntries({}),
      /*leader_commit=*/0,
      /*follower_id=*/1);
  auto ae_message = CreateAeMessage(ae_fields);

  bool ae_success = raft_->ReceiveAppendEntries(
      std::make_unique<AppendEntries>(std::move(ae_message)));
  EXPECT_TRUE(ae_success);

  auto req = std::make_unique<Request>();
  req->set_seq(1);
  bool success = raft_->ReceiveTransaction(std::move(req));
  EXPECT_FALSE(success);
}

// Test 2: A leader receiving a client transaction should send an AppendEntries
// to all other replicas.
TEST_F(RaftTest, LeaderSendsAppendEntriesUponClientTransaction) {
  EXPECT_CALL(mock_call, Call(_, _, _)).Times(3);
  EXPECT_CALL(*leader_election_manager_, OnAeBroadcast()).Times(1);

  auto req = std::make_unique<Request>();
  req->set_seq(1);
  raft_->SetStateForTest({.current_term = 0,
                          .role = Role::LEADER,
                          .log = CreateLogEntries({}, true),
                          .enable_batching = false});

  bool success = raft_->ReceiveTransaction(std::move(req));
  EXPECT_TRUE(success);
}

// Test 3: Sent AppendEntries should be based on the follower's next_index.
TEST_F(RaftTest, LeaderSendsAppendEntriesBasedOnNextIndex) {
  EXPECT_CALL(mock_call, Call(_, _, _))
      .WillOnce(::testing::Invoke(
          [](int type, const google::protobuf::Message& msg, int node_id) {
            const auto& ae = dynamic_cast<const AppendEntries&>(msg);
            EXPECT_EQ(node_id, 2);
            EXPECT_EQ(ae.prev_log_index(), 2);
            EXPECT_EQ(ae.entries().size(), 3);
            return 0;
          }))
      .WillOnce(::testing::Invoke(
          [](int type, const google::protobuf::Message& msg, int node_id) {
            const auto& ae = dynamic_cast<const AppendEntries&>(msg);
            EXPECT_EQ(node_id, 3);
            EXPECT_EQ(ae.prev_log_index(), 1);
            EXPECT_EQ(ae.entries().size(), 4);
            return 0;
          }))
      .WillOnce(::testing::Invoke(
          [](int type, const google::protobuf::Message& msg, int node_id) {
            const auto& ae = dynamic_cast<const AppendEntries&>(msg);
            EXPECT_EQ(node_id, 4);
            EXPECT_EQ(ae.prev_log_index(), 0);
            EXPECT_EQ(ae.entries().size(), 5);
            return 0;
          }));
  EXPECT_CALL(*leader_election_manager_, OnAeBroadcast()).Times(1);

  raft_->SetStateForTest(
      {.current_term = 0,
       .role = Role::LEADER,
       .log = CreateLogEntries(
           {
               {0, "Term 0 Transaction 1"},
               {0, "Term 0 Transaction 2"},
               {0, "Term 0 Transaction 3"},
               {0, "Term 0 Transaction 4"},
           },
           true),
       CreateProgressPatch({
           .next_index = std::vector<uint64_t>{1, 4, 3, 2, 1},
       }),
       .enable_batching = false});

  auto req = std::make_unique<Request>();
  req->set_seq(5);

  bool success = raft_->ReceiveTransaction(std::move(req));
  EXPECT_TRUE(success);
}

}  // namespace raft
}  // namespace resdb
