#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Emu/Cell/lv2/sys_net/psas_connector.h"

using namespace sys_net_helpers;

namespace
{
	std::vector<u8> bytes_of(std::string_view str)
	{
		return std::vector<u8>(str.begin(), str.end());
	}

	std::vector<u8> concat(std::initializer_list<std::vector<u8>> parts)
	{
		std::vector<u8> out;

		for (const auto& part : parts)
		{
			out.insert(out.end(), part.begin(), part.end());
		}

		return out;
	}
}

TEST(PsasConnector, RetailTitleGate)
{
	// The nine PS3 retail ids (connector docs/emulator-detection.md, "PSASBR title ids for the fork gate")
	static_assert(PSAS_RETAIL_TITLE_IDS.size() == 9);

	for (const char* id : {"BCES01435", "BCES01436", "BCUS98472", "BCJS30088", "BCAS20253", "NPUA80316", "NPEA00413", "NPJA00088", "NPHA80231"})
	{
		EXPECT_TRUE(is_psas_retail_title(id)) << id;
	}

	// Betas, demo, manual, placeholder, prototype, Vita ids, RPCS3 patch folder names and near misses are not gated
	for (const char* id : {"NPUA70214", "NPUA70246", "BCET70052", "BCET70053", "NPUA70232", "NPUO70233", "BCUS00000", "BCUS80316",
		"PCSA00069", "PCSF00153", "PCSC00032", "PCSD00040", "BCES01435111", "bces01435", "BCES0143", " BCES01435", ""})
	{
		EXPECT_FALSE(is_psas_retail_title(id)) << id;
	}

	static_assert(is_psas_retail_title("BCUS98472"));
	static_assert(!is_psas_retail_title("NPUA70246"));
}

// The expected strings below spell out PSAS_FORK_NAME / PSAS_FORK_REVISION on purpose:
// update them when the revision is bumped.
TEST(PsasConnector, ForkVersionComposition)
{
	static_assert(PSAS_FORK_NAME == "rpsascs3");
	static_assert(PSAS_FORK_REVISION == 1);

	// Local build (this tree) and CI-style upstream strings fit whole, release type included
	EXPECT_EQ(compose_psas_fork_version("0.0.42-2378b3c0 Alpha"), "rpsascs3 r1 0.0.42-2378b3c0 Alpha");
	EXPECT_EQ(compose_psas_fork_version("0.0.42-17345-abcdef12 Alpha"), "rpsascs3 r1 0.0.42-17345-abcdef12 Alpha");

	// Over 40 bytes: the release type goes, the build hash stays
	EXPECT_EQ(compose_psas_fork_version("0.0.42-17345-abcdef12 Alpha 2"), "rpsascs3 r1 0.0.42-17345-abcdef12");

	// Name and revision always survive the encoder's 40-byte clamp
	const std::string very_long = compose_psas_fork_version(std::string(60, 'x'));
	const std::vector<u8> datagram = encode_psas_connector_hello({.fork_version = very_long});

	ASSERT_EQ(datagram[10], PSAS_HELLO_MAX_FORK_VERSION);
	EXPECT_EQ(std::string(datagram.begin() + 11, datagram.begin() + 11 + 12), "rpsascs3 r1 ");
}

TEST(PsasConnector, HelloWireFormat)
{
	const std::string fork_version = compose_psas_fork_version("0.0.42-2378b3c0 Alpha");

	const std::vector<u8> datagram = encode_psas_connector_hello({
		.flags = PSAS_HELLO_FLAG_LOOPBACK_BIND | PSAS_HELLO_FLAG_FORWARD | PSAS_HELLO_FLAG_TITLE_GATE,
		.interval_ms = 2000,
		.fork_version = fork_version,
		.title_id = "BCES01435",
		.bound_addr = "127.0.0.1:3658",
	});

	const std::vector<u8> expected = concat({
		{'R', 'P', 'S', 'B'}, // magic
		{1},                  // hello version
		{1},                  // emulator: RPCS3
		{0x0B},               // flags: loopback bind | forward | title gate
		{0},                  // reserved
		{0xD0, 0x07},         // interval 2000 ms, little endian
		{33}, bytes_of("rpsascs3 r1 0.0.42-2378b3c0 Alpha"),
		{9}, bytes_of("BCES01435"),
		{14}, bytes_of("127.0.0.1:3658"),
	});

	EXPECT_EQ(datagram, expected);
}

TEST(PsasConnector, HelloOptOutWithoutBind)
{
	// Opt-out (checkbox off), stock config: forward inactive, nothing bound yet, empty bound address
	const std::vector<u8> datagram = encode_psas_connector_hello({
		.flags = PSAS_HELLO_FLAG_OPT_OUT | PSAS_HELLO_FLAG_TITLE_GATE,
		.fork_version = "v",
		.title_id = "NPUA80316",
	});

	const std::vector<u8> expected = concat({
		{'R', 'P', 'S', 'B', 1, 1, 0x0C, 0, 0xD0, 0x07},
		{1}, bytes_of("v"),
		{9}, bytes_of("NPUA80316"),
		{0},
	});

	EXPECT_EQ(datagram, expected);
}

TEST(PsasConnector, HelloMinimumSizeAndEndianness)
{
	const std::vector<u8> datagram = encode_psas_connector_hello({.interval_ms = 0x1234});

	ASSERT_EQ(datagram.size(), PSAS_HELLO_MIN_SIZE);
	EXPECT_EQ(datagram[8], 0x34);
	EXPECT_EQ(datagram[9], 0x12);
	EXPECT_EQ(datagram[10], 0);
	EXPECT_EQ(datagram[11], 0);
	EXPECT_EQ(datagram[12], 0);
}

TEST(PsasConnector, HelloForkVersionClamp)
{
	// Longer than 40 bytes: clamped to 40
	const std::string long_version(50, 'x');
	const std::vector<u8> datagram = encode_psas_connector_hello({.fork_version = long_version, .title_id = "BCUS98472"});

	ASSERT_EQ(datagram.size(), PSAS_HELLO_MIN_SIZE + PSAS_HELLO_MAX_FORK_VERSION + 9);
	EXPECT_EQ(datagram[10], PSAS_HELLO_MAX_FORK_VERSION);
	EXPECT_EQ(datagram[10 + 1 + PSAS_HELLO_MAX_FORK_VERSION], 9);

	// A multibyte UTF-8 character across the 40-byte limit is dropped whole, never split
	const std::string utf8_version = std::string(39, 'x') + "\xC2\xB7" + "tail";
	const std::vector<u8> clamped = encode_psas_connector_hello({.fork_version = utf8_version});

	EXPECT_EQ(clamped[10], 39);
	EXPECT_EQ(clamped.size(), PSAS_HELLO_MIN_SIZE + 39);
}
