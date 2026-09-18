/**
 * @file test_telemetry_proto.cpp
 * @brief Unit tests verifying Protobuf telemetry serialization and deserialization.
 */

#include "telemetry.pb.h"
#include <gtest/gtest.h>
#include <string>

TEST(TelemetryProtoTest, SnapshotSerializationRoundtrip)
{
    hft::telemetry::TelemetrySnapshot original;
    original.set_timestamp_ns(1710000000000ULL);
    original.set_elapsed_sec(5.1234);
    original.set_throughput_msg_sec(1250000.50);
    original.set_avg_latency_ns(142.75);
    original.set_min_latency_ns(95);
    original.set_max_latency_ns(1820);
    original.set_spsc_queue_depth(42);
    original.set_rx_frames_captured(500000);
    original.set_total_processed(499990);
    original.set_risk_approved(450000);
    original.set_risk_rejected(49990);
    original.set_approved_avg_latency_ns(138.20);
    original.set_rejected_avg_latency_ns(165.40);
    original.set_protocol(hft::telemetry::ProtocolType::PROTOCOL_FIX);

    // Serialize to binary string
    std::string serialized;
    ASSERT_TRUE(original.SerializeToString(&serialized));
    ASSERT_FALSE(serialized.empty());

    // Deserialize into new object
    hft::telemetry::TelemetrySnapshot decoded;
    ASSERT_TRUE(decoded.ParseFromString(serialized));

    // Verify field fidelity
    EXPECT_EQ(decoded.timestamp_ns(), 1710000000000ULL);
    EXPECT_DOUBLE_EQ(decoded.elapsed_sec(), 5.1234);
    EXPECT_DOUBLE_EQ(decoded.throughput_msg_sec(), 1250000.50);
    EXPECT_DOUBLE_EQ(decoded.avg_latency_ns(), 142.75);
    EXPECT_EQ(decoded.min_latency_ns(), 95ULL);
    EXPECT_EQ(decoded.max_latency_ns(), 1820ULL);
    EXPECT_EQ(decoded.spsc_queue_depth(), 42ULL);
    EXPECT_EQ(decoded.rx_frames_captured(), 500000ULL);
    EXPECT_EQ(decoded.total_processed(), 499990ULL);
    EXPECT_EQ(decoded.risk_approved(), 450000ULL);
    EXPECT_EQ(decoded.risk_rejected(), 49990ULL);
    EXPECT_DOUBLE_EQ(decoded.approved_avg_latency_ns(), 138.20);
    EXPECT_DOUBLE_EQ(decoded.rejected_avg_latency_ns(), 165.40);
    EXPECT_EQ(decoded.protocol(), hft::telemetry::ProtocolType::PROTOCOL_FIX);
}
