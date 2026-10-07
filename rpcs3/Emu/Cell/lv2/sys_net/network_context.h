#pragma once

#include <vector>
#include <map>
#include <string>
#include "Utilities/mutex.h"
#include "Emu/Cell/PPUThread.h"

#include "nt_p2p_port.h"

struct base_network_thread
{
	void add_ppu_to_awake(ppu_thread* ppu);
	void del_ppu_to_awake(ppu_thread* ppu);

	shared_mutex mutex_ppu_to_awake;
	std::vector<ppu_thread*> ppu_to_awake;
	shared_mutex mutex_thread_loop;

	void wake_threads();
};

struct network_thread : base_network_thread
{
	atomic_t<u32> num_polls = 0;

	static constexpr auto thread_name = "Network Thread";

	void operator()();
};

struct p2p_thread : base_network_thread
{
	shared_mutex list_p2p_ports_mutex;
	std::map<u16, nt_p2p_port> list_p2p_ports;
	atomic_t<u32> num_p2p_ports = 0;

	static constexpr auto thread_name = "Network P2P Thread";

	p2p_thread();

	void create_p2p_port(u16 p2p_port);

	void bind_sce_np_port();
	void operator()();
};

using network_context = named_thread<network_thread>;
using p2p_context = named_thread<p2p_thread>;

// ASBR connector mode (psas_connector.h): sends the hello datagram to the All-Stars Matchmaker
// connector every 2 s while a PSASBR retail title runs, from a plain host UDP socket (not a game socket).
// Created by init_psas_connector_hello() for gated titles only (not default-constructible, so the fxo
// init never creates it on its own), aborted and joined by the fxo reset when the emulation stops.
struct psas_connector_hello_thread
{
	static constexpr auto thread_name = "PSAS Connector Hello";

	explicit psas_connector_hello_thread(std::string title_id);

	void operator()();

private:
	const std::string m_title_id;
};

using psas_connector_hello_context = named_thread<psas_connector_hello_thread>;

// Starts the hello sender when the running title is a PSASBR retail title (once per executable boot)
void init_psas_connector_hello();
