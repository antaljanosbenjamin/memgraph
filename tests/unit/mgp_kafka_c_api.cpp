#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "integrations/kafka/consumer.hpp"
#include "query/procedure/mg_procedure_impl.hpp"
#include "test_utils.hpp"
#include "utils/pmr/vector.hpp"

/// This class implements the interface of RdKafka::Message such that it can be mocked.
/// It's important to note that integrations::kafka::Message member functions
/// use c_ptr() to indirectly access the results inside the rd_kafka_message_s structure
/// effectively bypassing the mocked values returned by the overrides below. Therefore, to
/// protect against accidental use of the public members, the functions are marked as
/// [[noreturn]] and throw an std::logic_error exception.
class MockedRdKafkaMessage : public RdKafka::Message {
 public:
  explicit MockedRdKafkaMessage(std::string key, std::string payload)
      : key_(std::move(key)), payload_(std::move(payload)) {
    message_.err = rd_kafka_resp_err_t::RD_KAFKA_RESP_ERR__BEGIN;
    message_.key = static_cast<void *>(key_.data());
    message_.key_len = key_.size();
    message_.offset = 0;
    message_.payload = static_cast<void *>(payload_.data());
    message_.len = payload_.size();
    rd_kafka_ = rd_kafka_new(rd_kafka_type_t::RD_KAFKA_CONSUMER, nullptr, nullptr, 0);
    message_.rkt = rd_kafka_topic_new(rd_kafka_, mocked_topic_name.data(), nullptr);
  }

  ~MockedRdKafkaMessage() override {
    rd_kafka_destroy(rd_kafka_);
    rd_kafka_topic_destroy(message_.rkt);
  }

  // The two can be accessed safely. Any use of the other public members should
  // be considered accidental (as per the current semantics of the class
  // Message) and therefore they are marked as [[noreturn]] and throw
  rd_kafka_message_s *c_ptr() override { return &message_; }

  // This is used by Message() constructor

  RdKafka::ErrorCode err() const override { return RdKafka::ErrorCode::ERR_NO_ERROR; }

  [[noreturn]] std::string errstr() const override { ThrowIllegalCallError(); }

  [[noreturn]] RdKafka::Topic *topic() const override { ThrowIllegalCallError(); }

  [[noreturn]] std::string topic_name() const override { ThrowIllegalCallError(); }

  [[noreturn]] int32_t partition() const override { ThrowIllegalCallError(); }

  [[noreturn]] void *payload() const override { ThrowIllegalCallError(); }

  [[noreturn]] size_t len() const override { ThrowIllegalCallError(); }

  [[noreturn]] const std::string *key() const override { ThrowIllegalCallError(); }

  [[noreturn]] const void *key_pointer() const override { ThrowIllegalCallError(); }

  [[noreturn]] size_t key_len() const override { ThrowIllegalCallError(); }

  [[noreturn]] int64_t offset() const override { ThrowIllegalCallError(); }

  [[noreturn]] RdKafka::MessageTimestamp timestamp() const override { ThrowIllegalCallError(); }

  [[noreturn]] void *msg_opaque() const override { ThrowIllegalCallError(); }

  [[noreturn]] int64_t latency() const override { ThrowIllegalCallError(); }

  [[noreturn]] Status status() const override { ThrowIllegalCallError(); }

  [[noreturn]] RdKafka::Headers *headers() override { ThrowIllegalCallError(); }

  [[noreturn]] RdKafka::Headers *headers(RdKafka::ErrorCode *err) override { ThrowIllegalCallError(); }

  [[noreturn]] int32_t broker_id() const override { ThrowIllegalCallError(); }

 private:
  [[noreturn]] void ThrowIllegalCallError() const {
    throw std::logic_error("This function should not have been called");
  }

  std::string key_;
  rd_kafka_t *rd_kafka_;
  std::string payload_;
  rd_kafka_message_s message_;

  static std::string mocked_topic_name;
};

std::string MockedRdKafkaMessage::mocked_topic_name = "Topic1";

class MgpApiTest : public ::testing::Test {
 public:
  using Message = integrations::kafka::Message;
  using KafkaMessage = MockedRdKafkaMessage;

  MgpApiTest() { messages_.emplace(CreateMockedBatch()); }
  ~MgpApiTest() { messages_.reset(); }

  mgp_messages &Messages() { return *messages_; }

 protected:
  struct ExpectedResult {
    const char *payload;
    const char key;
    const char *topic_name;
    const size_t payload_size;
  };

  static constexpr std::array<ExpectedResult, 2> expected = {ExpectedResult{"payload1", '1', "Topic1", 8},
                                                             ExpectedResult{"payload2", '2', "Topic1", 8}};

 private:
  utils::pmr::vector<mgp_message> CreateMockedBatch() {
    std::transform(expected.begin(), expected.end(), std::back_inserter(msgs_storage_), [](const auto expected) {
      return Message(std::make_unique<KafkaMessage>(std::string(1, expected.key), expected.payload));
    });
    auto v = utils::pmr::vector<mgp_message>(utils::NewDeleteResource());
    v.reserve(expected.size());
    std::transform(msgs_storage_.begin(), msgs_storage_.end(), std::back_inserter(v),
                   [](auto &msgs) { return mgp_message{&msgs}; });
    return v;
  }

  utils::pmr::vector<Message> msgs_storage_{utils::NewDeleteResource()};
  std::optional<mgp_messages> messages_;
};

TEST_F(MgpApiTest, TestAllMgpKafkaCApi) {
  mgp_messages &messages = Messages();
  EXPECT_EQ(EXPECT_MGP_NO_ERROR(size_t, mgp_messages_size, &messages), expected.size());

  for (int i = 0; i < expected.size(); ++i) {
    auto *message = EXPECT_MGP_NO_ERROR(mgp_message *, mgp_messages_at, &messages, i);
    // Test for key and key size. Key size is always 1 in this test.
    EXPECT_EQ(EXPECT_MGP_NO_ERROR(size_t, mgp_message_key_size, message), 1);
    EXPECT_EQ(*EXPECT_MGP_NO_ERROR(const char *, mgp_message_key, message), expected[i].key);

    // Test for payload size
    EXPECT_EQ(EXPECT_MGP_NO_ERROR(size_t, mgp_message_payload_size, message), expected[i].payload_size);
    // Test for payload
    EXPECT_FALSE(std::strcmp(EXPECT_MGP_NO_ERROR(const char *, mgp_message_payload, message), expected[i].payload));
    // Test for topic name
    EXPECT_FALSE(
        std::strcmp(EXPECT_MGP_NO_ERROR(const char *, mgp_message_topic_name, message), expected[i].topic_name));
  }

  // Unfortunately, we can't test timestamp here because we can't mock (as explained above)
  // and the test does not have access to the internal rd_kafka_message2msg() function.
  // auto expected_timestamp = rd_kafka_timestamp_type_t::RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
  // EXPECT_EQ(mgp_message_timestamp(first_msg), expected_timestamp);
  // EXPECT_EQ(mgp_message_timestamp(second_msg), expected_timestamp);
}
