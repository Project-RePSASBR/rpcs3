#pragma once

// ASBR connector mode (rpsascs3 fork)
//
// For PlayStation All-Stars Battle Royale (the nine PS3 retail title ids below, nothing else) the emulator
// binds its game sockets to 127.0.0.1 and forwards unicast copies of every P2P broadcast to the All-Stars
// Matchmaker connector on 127.0.0.1:4000, whatever "Bind address" and "P2P Broadcast Forward" say
// (resolve_binding_ip() in sys_net_helpers.cpp, lv2_socket_p2p::sendto()). While such a title runs, a hello
// datagram tells the connector which emulator is talking to it (psas_connector_hello_thread in
// network_context.cpp). The user can opt out with the "PSAS Connector Mode" setting (Network tab), which
// restores stock networking; the hello is still sent, flagged as opt-out, so the connector can tell why it
// sees no game traffic. Other titles are not affected in any way.
//
// Design note: docs/emulator-detection.md in the connector repository (appendices "hello datagram wire
// format (fork -> connector)" and "PSASBR title ids for the fork gate").

#include "util/types.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sys_net_helpers
{
	// UDP port of the All-Stars Matchmaker connector (beacon port) on the local machine
	constexpr u16 PSAS_CONNECTOR_PORT = 4000;

	// Hello datagram (fork -> connector)
	constexpr u16 PSAS_HELLO_INTERVAL_MS      = 2000;
	constexpr u8 PSAS_HELLO_VERSION           = 1;
	constexpr u8 PSAS_HELLO_EMULATOR_RPCS3    = 1;
	constexpr usz PSAS_HELLO_MAX_FORK_VERSION = 40;
	constexpr usz PSAS_HELLO_MIN_SIZE         = 13;

	// Fork identity sent in the hello's fork version field, so the connector can tell this fork apart from
	// upstream RPCS3 and from older revisions of it. PSAS_FORK_REVISION is bumped BY HAND whenever the fork's
	// connector-relevant behaviour changes (forced bind/forward rules, hello contents, P2P framing).
	constexpr std::string_view PSAS_FORK_NAME = "rpsascs3";
	constexpr u32 PSAS_FORK_REVISION = 1;

	enum PSAS_HELLO_FLAGS : u8
	{
		PSAS_HELLO_FLAG_LOOPBACK_BIND = 1 << 0, // the game's P2P port is bound to 127.0.0.1 by connector mode
		PSAS_HELLO_FLAG_FORWARD       = 1 << 1, // broadcasts are forwarded (connector mode, or a configured P2P Broadcast Forward list)
		PSAS_HELLO_FLAG_OPT_OUT       = 1 << 2, // connector mode disabled by the user
		PSAS_HELLO_FLAG_TITLE_GATE    = 1 << 3, // the running title is a PSASBR retail title
	};

	// PS3 retail title ids of PlayStation All-Stars Battle Royale. Betas, demos and prototypes (NPUA70214,
	// NPUA70246, BCET70052, BCET70053, NPUA70232, BCUS00000, BCUS80316) are deliberately not gated: they run
	// older protocol ids and cannot play with retail 1.12 anyway.
	constexpr std::array<std::string_view, 9> PSAS_RETAIL_TITLE_IDS
	{
		"BCES01435", // disc, Europe / Australia (PAL SKU 1)
		"BCES01436", // disc, Europe (PAL SKU 2)
		"BCUS98472", // disc, USA / Canada
		"BCJS30088", // disc, Japan
		"BCAS20253", // disc, Asia
		"NPUA80316", // PSN, USA
		"NPEA00413", // PSN, Europe
		"NPJA00088", // PSN, Japan
		"NPHA80231", // PSN, Asia
	};

	constexpr bool is_psas_retail_title(std::string_view title_id)
	{
		for (const std::string_view id : PSAS_RETAIL_TITLE_IDS)
		{
			if (id == title_id)
			{
				return true;
			}
		}

		return false;
	}

	struct psas_connector_hello
	{
		u8 flags = 0;
		u16 interval_ms = PSAS_HELLO_INTERVAL_MS;
		std::string_view fork_version; // clamped to PSAS_HELLO_MAX_FORK_VERSION bytes
		std::string_view title_id;
		std::string_view bound_addr;   // "127.0.0.1:3658", empty while the P2P port is not bound in connector mode
	};

	// Builds the hello datagram (pure function, little endian, fixed layout):
	// "RPSB" | hello version u8 | emulator u8 | flags u8 | reserved u8 (0) | interval u16 |
	// fork version (u8 length + UTF-8) | title id (u8 length + ASCII) | bound game address (u8 length + ASCII)
	inline std::vector<u8> encode_psas_connector_hello(const psas_connector_hello& hello)
	{
		// Clamp a string to max_len bytes without splitting a UTF-8 sequence
		const auto clamp = [](std::string_view str, usz max_len) -> std::string_view
		{
			if (str.size() <= max_len)
			{
				return str;
			}

			usz len = max_len;

			while (len && (static_cast<u8>(str[len]) & 0xC0) == 0x80)
			{
				len--;
			}

			return str.substr(0, len);
		};

		const std::array<std::string_view, 3> fields
		{
			clamp(hello.fork_version, PSAS_HELLO_MAX_FORK_VERSION),
			clamp(hello.title_id, 0xFF),
			clamp(hello.bound_addr, 0xFF),
		};

		std::vector<u8> out;
		out.reserve(PSAS_HELLO_MIN_SIZE + fields[0].size() + fields[1].size() + fields[2].size());

		out.push_back('R');
		out.push_back('P');
		out.push_back('S');
		out.push_back('B');
		out.push_back(PSAS_HELLO_VERSION);
		out.push_back(PSAS_HELLO_EMULATOR_RPCS3);
		out.push_back(hello.flags);
		out.push_back(0); // reserved
		out.push_back(static_cast<u8>(hello.interval_ms & 0xFF));
		out.push_back(static_cast<u8>(hello.interval_ms >> 8));

		for (const std::string_view field : fields)
		{
			out.push_back(static_cast<u8>(field.size()));

			for (const char c : field)
			{
				out.push_back(static_cast<u8>(c));
			}
		}

		return out;
	}

	// Hello fork version: "<fork name> r<revision> <RPCS3 version string>", e.g. "rpsascs3 r1 0.0.42-17345-abcdef12 Alpha".
	// Name and revision come first so they always survive the 40-byte limit; when the whole string would not fit,
	// the upstream version loses its release-type suffix (" Alpha", " Alpha 2", ...) so the build hash stays in.
	inline std::string compose_psas_fork_version(std::string_view upstream_version)
	{
		const std::string prefix = std::string(PSAS_FORK_NAME) + " r" + std::to_string(PSAS_FORK_REVISION) + " ";

		if (prefix.size() + upstream_version.size() > PSAS_HELLO_MAX_FORK_VERSION)
		{
			// The release type is everything after the first space ("0.0.42-17345-abcdef12 Alpha")
			if (const usz space = upstream_version.find(' '); space != std::string_view::npos)
			{
				upstream_version = upstream_version.substr(0, space);
			}
		}

		// Anything still too long is clamped at the end by encode_psas_connector_hello()
		return prefix + std::string(upstream_version);
	}

	// Runtime gate (nt_p2p_port.cpp)

	// The running title (Emu.GetTitleID(), read from PARAM.SFO) is a PSASBR retail title
	bool is_psas_title_gate_matched();

	// Title gate matched and "PSAS Connector Mode" enabled: force the loopback bind and the connector forward
	bool psas_connector_mode_active();

	// Game P2P port bound to 127.0.0.1 by connector mode (3658 for PSASBR), 0 while there is none
	u16 get_psas_connector_bound_port();
}
