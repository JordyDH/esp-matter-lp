// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream.h"

#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_chrono/simulated_system_clock.h"

namespace bt::iso {
namespace {

using pw::bluetooth::emboss::IsoDataPacketStatus;
using pw::bluetooth::emboss::IsoDataPbFlag;

constexpr hci_spec::CigIdentifier kCigId = 0x22;
constexpr hci_spec::CisIdentifier kCisId = 0x42;

constexpr hci_spec::ConnectionHandle kCisHandleId = 0x59e;

constexpr size_t kMaxControllerSDUFragmentSize = 100;
constexpr size_t kMaxControllerPacketCount = 9;

constexpr size_t kSduHeaderSize = 4;

using MockControllerTestBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

class IsoStreamTest : public MockControllerTestBase {
 public:
  IsoStreamTest() = default;
  ~IsoStreamTest() override = default;

  void SetUp() override {
    MockControllerTestBase::SetUp(
        pw::bluetooth::Controller::FeaturesBits::kHciIso);
    hci::DataBufferInfo iso_buffer_info(kMaxControllerSDUFragmentSize,
                                        kMaxControllerPacketCount);
    transport()->InitializeIsoDataChannel(iso_buffer_info);
    iso_stream_ = IsoStream::Create(
        kCigId,
        kCisId,
        kCisHandleId,
        /*on_established_cb=*/
        [this](pw::bluetooth::emboss::StatusCode status,
               std::optional<WeakSelf<IsoStream>::WeakPtr>,
               const std::optional<CisEstablishedParameters>& parameters) {
          ASSERT_FALSE(establishment_status_.has_value());
          establishment_status_ = status;
          established_parameters_ = parameters;
        },
        transport()->command_channel()->AsWeakPtr(),
        /*on_closed_cb=*/
        [this]() {
          ASSERT_FALSE(closed_);
          closed_ = true;
        },
        transport()->iso_data_channel(),
        test_clock_);
  }

 protected:
  // Send an HCI_LE_CIS_Established event with the provided status
  void EstablishCis(pw::bluetooth::emboss::StatusCode status);

  // Call IsoStream::SetupDataPath().
  // |cmd_complete_status| is nullopt if we do not expect an
  // LE_Setup_ISO_Data_Path command to be generated, otherwise it should be set
  // to the status code we want to generate in the response frame.
  // |expected_cb_result| should be set to the expected result of the callback
  // from IsoStream::SetupDataPath.
  void SetupDataPath(pw::bluetooth::emboss::DataPathDirection direction,
                     const std::optional<std::vector<uint8_t>>& codec_config,
                     const std::optional<pw::bluetooth::emboss::StatusCode>&
                         cmd_complete_status,
                     iso::IsoStream::SetupDataPathError expected_cb_result,
                     bool generate_mismatched_cid = false);

  void RegisterStream() {
    transport()->iso_data_channel()->RegisterConnection(
        kCisHandleId, iso_stream()->GetWeakPtr());
  }

  bool HandleCompleteIncomingSDU(const pw::span<const std::byte>& complete_sdu);

  IsoStream* iso_stream() { return iso_stream_.get(); }

  std::queue<std::vector<std::byte>>* complete_incoming_sdus() {
    return &complete_incoming_sdus_;
  }

  std::optional<pw::bluetooth::emboss::StatusCode> establishment_status() {
    return establishment_status_;
  }

  std::optional<CisEstablishedParameters> established_parameters() {
    return established_parameters_;
  }

  bool closed() { return closed_; }

 protected:
  bool accept_incoming_sdus_ = true;
  pw::chrono::SimulatedSystemClock test_clock_;

 private:
  std::unique_ptr<IsoStream> iso_stream_;
  std::optional<pw::bluetooth::emboss::StatusCode> establishment_status_;
  std::optional<CisEstablishedParameters> established_parameters_;
  std::queue<std::vector<std::byte>> complete_incoming_sdus_;
  bool closed_ = false;
};

static DynamicByteBuffer LECisEstablishedPacketWithDefaultValues(
    pw::bluetooth::emboss::StatusCode status) {
  return testing::LECisEstablishedEventPacket(
      status,
      kCisHandleId,
      /*cig_sync_delay_us=*/0x123456,  // Must be in [0x0000ea, 0x7fffff]
      /*cis_sync_delay_us=*/0x7890ab,  // Must be in [0x0000ea, 0x7fffff]
      /*transport_latency_c_to_p_us=*/0x654321,  // Must be in [0x0000ea,
                                                 // 0x7fffff]
      /*transport_latency_p_to_c_us=*/0x0fedcb,  // Must be in [0x0000ea,
                                                 // 0x7fffff]
      /*phy_c_to_p=*/pw::bluetooth::emboss::IsoPhyType::LE_2M,
      /*phy_c_to_p=*/pw::bluetooth::emboss::IsoPhyType::LE_CODED,
      /*nse=*/0x10,               // Must be in [0x01, 0x1f]
      /*bn_c_to_p=*/0x05,         // Must be in [0x00, 0x0f]
      /*bn_p_to_c=*/0x0f,         // Must be in [0x00, 0x0f]
      /*ft_c_to_p=*/0x01,         // Must be in [0x01, 0xff]
      /*ft_p_to_c=*/0xff,         // Must be in [0x01, 0xff]
      /*max_pdu_c_to_p=*/0x0042,  // Must be in [0x0000, 0x00fb]
      /*max_pdu_p_to_c=*/0x00fb,  // Must be in [0x0000, 0x00fb]
      /*iso_interval=*/0x0222     // Must be in [0x0004, 0x0c80]
  );
}

void IsoStreamTest::EstablishCis(pw::bluetooth::emboss::StatusCode status) {
  DynamicByteBuffer le_cis_established_packet =
      LECisEstablishedPacketWithDefaultValues(status);
  test_device()->SendCommandChannelPacket(le_cis_established_packet);
  RunUntilIdle();
  ASSERT_TRUE(establishment_status().has_value());
  EXPECT_EQ(*(establishment_status()), status);
  if (status == pw::bluetooth::emboss::StatusCode::SUCCESS) {
    EXPECT_TRUE(established_parameters().has_value());
  } else {
    EXPECT_FALSE(established_parameters().has_value());
  }
}

bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> GenerateCodecId() {
  const uint16_t kCompanyId = 0x1234;
  bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id;
  auto codec_id_view = codec_id.view();
  codec_id_view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::LC3);
  codec_id_view.company_id().Write(kCompanyId);
  return codec_id;
}

void IsoStreamTest::SetupDataPath(
    pw::bluetooth::emboss::DataPathDirection direction,
    const std::optional<std::vector<uint8_t>>& codec_configuration,
    const std::optional<pw::bluetooth::emboss::StatusCode>& cmd_complete_status,
    iso::IsoStream::SetupDataPathError expected_cb_result,
    bool generate_mismatched_cid) {
  const uint32_t kControllerDelay = 1234;  // Must be < 4000000
  std::optional<iso::IsoStream::SetupDataPathError> actual_cb_result;

  if (cmd_complete_status.has_value()) {
    auto setup_data_path_packet =
        testing::LESetupIsoDataPathPacket(kCisHandleId,
                                          direction,
                                          /*HCI*/ 0,
                                          GenerateCodecId(),
                                          kControllerDelay,
                                          codec_configuration);
    hci_spec::ConnectionHandle cis_handle =
        kCisHandleId + (generate_mismatched_cid ? 1 : 0);
    auto setup_data_path_complete_packet =
        testing::LESetupIsoDataPathResponse(*cmd_complete_status, cis_handle);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          setup_data_path_packet,
                          &setup_data_path_complete_packet);
  }

  iso_stream()->SetupDataPath(
      direction,
      GenerateCodecId(),
      codec_configuration,
      kControllerDelay,
      [&actual_cb_result](iso::IsoStream::SetupDataPathError result) {
        actual_cb_result = result;
      },
      fit::bind_member<&IsoStreamTest::HandleCompleteIncomingSDU>(this));
  RunUntilIdle();
  ASSERT_TRUE(actual_cb_result.has_value());
  EXPECT_EQ(expected_cb_result, *actual_cb_result);
}

bool IsoStreamTest::HandleCompleteIncomingSDU(
    const pw::span<const std::byte>& sdu) {
  if (!accept_incoming_sdus_) {
    return false;
  }
  std::vector<std::byte> new_sdu(sdu.size());
  std::copy(sdu.begin(), sdu.end(), new_sdu.begin());
  complete_incoming_sdus_.emplace(std::move(new_sdu));
  return true;
}

TEST_F(IsoStreamTest, CisEstablishedSuccessfully) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST_F(IsoStreamTest, CisEstablishmentFailed) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
}

TEST_F(IsoStreamTest, ClosedCallsCloseCallback) {
  EXPECT_FALSE(closed());
  iso_stream()->Close();
  EXPECT_TRUE(closed());
}

TEST_F(IsoStreamTest, SetupDataPathSuccessfully) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                pw::bluetooth::emboss::StatusCode::SUCCESS,
                iso::IsoStream::SetupDataPathError::kSuccess);
}

TEST_F(IsoStreamTest, SetupDataPathBeforeCisEstablished) {
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kCisNotEstablished);
}

TEST_F(IsoStreamTest, SetupInputDataPathTwice) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::INPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kStreamAlreadyExists);
}

TEST_F(IsoStreamTest, SetupOutputDataPathTwice) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kStreamAlreadyExists);
}

TEST_F(IsoStreamTest, SetupBothInputAndOutputDataPaths) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
}

TEST_F(IsoStreamTest, SetupDataPathInvalidArgs) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(static_cast<pw::bluetooth::emboss::DataPathDirection>(250),
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kInvalidArgs);
}

TEST_F(IsoStreamTest, SetupDataPathWithCodecConfig) {
  std::vector<uint8_t> codec_config{5, 6, 7, 8};
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      codec_config,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
}

// If the connection ID doesn't match in the command complete packet, fail
TEST_F(IsoStreamTest, SetupDataPathHandleMismatch) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kStreamRejectedByController,
      /*generate_mismatched_cid=*/true);
}

TEST_F(IsoStreamTest, SetupDataPathControllerError) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/
      pw::bluetooth::emboss::StatusCode::CONNECTION_ALREADY_EXISTS,
      iso::IsoStream::SetupDataPathError::kStreamRejectedByController);
}

// If the client asks for frames before any are ready it will receive
// a notification when the next packet arrives.
TEST_F(IsoStreamTest, PendingRead) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kIsoSduLength = 212;
  std::unique_ptr<std::vector<uint8_t>> sdu_data =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/14);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      *sdu_data);
  pw::span<const std::byte> packet0_as_span = packet0.subspan();
  ASSERT_EQ(iso_stream()->ReadNextQueuedIncomingPacket(), nullptr);
  iso_stream()->ReceiveInboundPacket(packet0_as_span);
  ASSERT_EQ(complete_incoming_sdus()->size(), 1u);
  std::vector<std::byte>& received_frame = complete_incoming_sdus()->front();
  ASSERT_EQ(packet0_as_span.size(), received_frame.size());
  EXPECT_TRUE(std::equal(
      packet0_as_span.begin(), packet0_as_span.end(), received_frame.begin()));
}

// If the client does not ask for frames it will not receive any notifications
// and the IsoStream will just queue them up.
TEST_F(IsoStreamTest, UnreadData) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kTotalFrameCount = 5;
  DynamicByteBuffer packets[kTotalFrameCount];
  pw::span<const std::byte> packets_as_span[kTotalFrameCount];
  for (size_t i = 0; i < kTotalFrameCount; i++) {
    std::unique_ptr<std::vector<uint8_t>> sdu_data = testing::GenDataBlob(
        /*size=*/kMaxControllerSDUFragmentSize - i, /*starting_value=*/i);
    packets[i] = testing::IsoDataPacket(
        /*connection_handle=*/iso_stream()->cis_handle(),
        pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*time_stamp=*/std::nullopt,
        /*packet_sequence_number=*/i,
        /*iso_sdu_length=*/kMaxControllerSDUFragmentSize - i,
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        *sdu_data);
    packets_as_span[i] = packets[i].subspan();
    iso_stream()->ReceiveInboundPacket(packets_as_span[i]);
  }
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);
}

// This is the (somewhat unusual) case where the client asks for a frame but
// then rejects it when the frame is ready. The frame should stay in the queue
// and future frames should not receive notification, either.
TEST_F(IsoStreamTest, ReadRequestedAndThenRejected) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  size_t packet0_sdu_fragment_size = kMaxControllerSDUFragmentSize;
  std::unique_ptr<std::vector<uint8_t>> packet0_sdu_data =
      testing::GenDataBlob(packet0_sdu_fragment_size, /*starting_value=*/11);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      /*iso_sdu_length=*/packet0_sdu_fragment_size,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      *packet0_sdu_data);
  pw::span<const std::byte> packet0_as_span = packet0.subspan();
  size_t packet1_sdu_fragment_size = packet0_sdu_fragment_size - 1;
  std::unique_ptr<std::vector<uint8_t>> packet1_sdu_data =
      testing::GenDataBlob(packet1_sdu_fragment_size, /*starting_value=*/13);
  DynamicByteBuffer packet1 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/1,
      /*iso_sdu_length=*/packet1_sdu_fragment_size,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      *packet1_sdu_data);
  pw::span<const std::byte> packet1_as_span = packet1.subspan();

  // Request a frame but then reject it when proffered by the stream
  ASSERT_EQ(iso_stream()->ReadNextQueuedIncomingPacket(), nullptr);
  accept_incoming_sdus_ = false;
  iso_stream()->ReceiveInboundPacket(packet0_as_span);
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);

  // Accept future frames, but because no read request has been made that we
  // couldn't fulfill, the stream should just queue them up.
  accept_incoming_sdus_ = true;
  iso_stream()->ReceiveInboundPacket(packet1_as_span);
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);

  // And finally, we should be able to read out the packets in the right order
  std::unique_ptr<IsoDataPacket> rx_packet_0 =
      iso_stream()->ReadNextQueuedIncomingPacket();
  ASSERT_NE(rx_packet_0, nullptr);
  ASSERT_EQ(rx_packet_0->size(), packet0_as_span.size());
  ASSERT_TRUE(std::equal(
      rx_packet_0->begin(), rx_packet_0->end(), packet0_as_span.begin()));
  std::unique_ptr<IsoDataPacket> rx_packet_1 =
      iso_stream()->ReadNextQueuedIncomingPacket();
  ASSERT_NE(rx_packet_1, nullptr);
  ASSERT_EQ(rx_packet_1->size(), packet1_as_span.size());
  ASSERT_TRUE(std::equal(
      rx_packet_1->begin(), rx_packet_1->end(), packet1_as_span.begin()));

  // Stream's packet queue should be empty now
  ASSERT_EQ(iso_stream()->ReadNextQueuedIncomingPacket(), nullptr);
}

// Bad packets will not be passed on
TEST_F(IsoStreamTest, BadPacket) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kIsoSduLength = 212;
  std::unique_ptr<std::vector<uint8_t>> sdu_data =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/91);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      *sdu_data);
  // Improperly formatted packets are discarded. To test this, we'll remove the
  // last byte of SDU data before we send it.
  pw::span<const std::byte> packet0_as_span =
      packet0.subspan(0, packet0.size() - 1);
  ASSERT_EQ(iso_stream()->ReadNextQueuedIncomingPacket(), nullptr);
  iso_stream()->ReceiveInboundPacket(packet0_as_span);
  ASSERT_EQ(complete_incoming_sdus()->size(), 0u);
}

// Extra data at the end of the frame will be removed
TEST_F(IsoStreamTest, ExcessDataIsTruncated) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kIsoSduLength = 212;
  std::unique_ptr<std::vector<uint8_t>> sdu_data =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/91);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      *sdu_data);
  size_t original_packet0_size = packet0.size();
  packet0.expand(packet0.size() + 100);
  pw::span<const std::byte> packet0_as_span = packet0.subspan();
  ASSERT_EQ(iso_stream()->ReadNextQueuedIncomingPacket(), nullptr);
  iso_stream()->ReceiveInboundPacket(packet0_as_span);
  ASSERT_EQ(complete_incoming_sdus()->size(), 1u);
  EXPECT_EQ(complete_incoming_sdus()->front().size(), original_packet0_size);
}

// Ensure sent packets are structured and sent correctly, including
// fragmentation.
TEST_F(IsoStreamTest, SendPacket) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  {
    std::vector<uint8_t> blob =
        *testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), blob.size())));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by one ISO interval
    test_clock_.AdvanceTime(std::chrono::microseconds(iso_interval_usec));
  }

  {
    // Send a packet that fits exactly within one ISO buffer easily.
    std::vector<uint8_t> blob =
        *testing::GenDataBlob(kMaxFirstPacketSize, /*starting_value=*/7);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), blob.size())));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by one ISO interval
    test_clock_.AdvanceTime(std::chrono::microseconds(iso_interval_usec));
  }

  {
    // Send a packet that has to be split into three.
    std::vector<uint8_t> blob = *testing::GenDataBlob(
        kMaxFirstPacketSize + kMaxControllerSDUFragmentSize * 2,
        /*starting_value=*/42);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::FIRST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), kMaxFirstPacketSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            /*sdu_data = */
            pw::span(blob.data(), blob.size())
                .subspan(kMaxFirstPacketSize, kMaxControllerSDUFragmentSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::LAST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            /*sdu_data = */
            pw::span(blob.data(), blob.size())
                .subspan(kMaxFirstPacketSize + kMaxControllerSDUFragmentSize)));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    expected_sequence_num += 2;

    // Advance the clock by two ISO intervals to simulate a skipped interval
    test_clock_.AdvanceTime(std::chrono::microseconds(2 * iso_interval_usec));
  }

  {
    // Send a packet that just barely needs to be split into three fragments.
    std::vector<uint8_t> blob = *testing::GenDataBlob(
        kMaxFirstPacketSize + kMaxControllerSDUFragmentSize + 1,
        /*starting_value=*/77);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::FIRST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), kMaxFirstPacketSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            /*sdu_data = */
            pw::span(blob.data(), blob.size())
                .subspan(kMaxFirstPacketSize, kMaxControllerSDUFragmentSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::LAST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            /*sdu_data = */
            pw::span(blob.data(), blob.size())
                .subspan(kMaxFirstPacketSize + kMaxControllerSDUFragmentSize)));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }
}

TEST_F(IsoStreamTest, PacketReceivedBeforeNextInterval) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  // Send the first packet
  {
    std::vector<uint8_t> blob1 =
        *testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);

    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob1.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob1.data(), blob1.size())));

    iso_stream()->Send(pw::span(
        reinterpret_cast<const std::byte*>(blob1.data()), blob1.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
    ++expected_sequence_num;
  }

  // Send a second packet immediately before the next interval is produced
  {
    std::vector<uint8_t> blob2 =
        *testing::GenDataBlob(kMaxFirstPacketSize / 3, /*starting_value=*/55);

    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob2.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob2.data(), blob2.size())));

    iso_stream()->Send(pw::span(
        reinterpret_cast<const std::byte*>(blob2.data()), blob2.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
    ++expected_sequence_num;

    test_clock_.AdvanceTime(std::chrono::microseconds(iso_interval_usec));
  }

  // Send a third packet after the interval has passed
  {
    std::vector<uint8_t> blob3 =
        *testing::GenDataBlob(kMaxFirstPacketSize / 4, /*starting_value=*/99);

    // Expect the sequence number to have advanced to the next interval
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob3.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob3.data(), blob3.size())));

    iso_stream()->Send(pw::span(
        reinterpret_cast<const std::byte*>(blob3.data()), blob3.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }
}

TEST_F(IsoStreamTest, PacketReceivedAtIntervalBoundaries) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  {
    std::vector<uint8_t> blob =
        *testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), blob.size())));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by almost one ISO interval
    test_clock_.AdvanceTime(std::chrono::microseconds(iso_interval_usec - 1));
  }

  {
    // Send a packet one tick before the next interval
    std::vector<uint8_t> blob =
        *testing::GenDataBlob(kMaxFirstPacketSize, /*starting_value=*/7);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob.data(), blob.size())));

    iso_stream()->Send(
        pw::span(reinterpret_cast<const std::byte*>(blob.data()), blob.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    // Advance the clock by one more microsecond to reach the next interval
    test_clock_.AdvanceTime(std::chrono::microseconds(1));
    ++expected_sequence_num;
  }

  // Send a third packet after the interval has passed
  {
    std::vector<uint8_t> blob3 =
        *testing::GenDataBlob(kMaxFirstPacketSize / 4, /*starting_value=*/99);

    // Expect the sequence number to have advanced to the next interval
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ blob3.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(blob3.data(), blob3.size())));

    iso_stream()->Send(pw::span(
        reinterpret_cast<const std::byte*>(blob3.data()), blob3.size()));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }
}

}  // namespace
}  // namespace bt::iso
