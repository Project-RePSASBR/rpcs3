#include "Emu/NP/ip_address.h"
#include "stdafx.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/Modules/sceNp.h" // for SCE_NP_PORT

#include "network_context.h"
#include "sys_net_helpers.h"
#include "psas_connector.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/NP/np_helpers.h"
#include "rpcs3_version.h"

#include <charconv>

LOG_CHANNEL(sys_net);

// Used by RPCN to send signaling packets to RPCN server(for UDP hole punching)
bool send_packet_from_p2p_port_ipv4(const std::vector<u8>& data, const sockaddr_in& addr)
{
	auto& nc = g_fxo->get<p2p_context>();
	{
		std::lock_guard list_lock(nc.list_p2p_ports_mutex);
		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);

			if (np::is_ipv6_supported())
			{
				const auto addr6 = np::sockaddr_to_sockaddr6(addr);

				if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr6), sizeof(sockaddr_in6)) == -1)
				{
					sys_net.error("Failed to send IPv4 signaling packet on IPv6 socket: %s", get_last_error(false, false));
					return false;
				}
			}
			else if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_in)) == -1)
			{
				sys_net.error("Failed to send signaling packet on IPv4 socket: %s", get_last_error(false, false));
				return false;
			}
		}
		else
		{
			sys_net.error("send_packet_from_p2p_port_ipv4: port %d not present", +SCE_NP_PORT);
			return false;
		}
	}

	return true;
}

bool send_packet_from_p2p_port_ipv6(const std::vector<u8>& data, const sockaddr_in6& addr)
{
	auto& nc = g_fxo->get<p2p_context>();
	{
		std::lock_guard list_lock(nc.list_p2p_ports_mutex);
		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);
			ensure(np::is_ipv6_supported());

			if (::sendto(def_port.p2p_socket, reinterpret_cast<const char*>(data.data()), ::size32(data), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(sockaddr_in6)) == -1)
			{
				sys_net.error("Failed to send signaling packet on IPv6 socket: %s", get_last_error(false, false));
				return false;
			}
		}
		else
		{
			sys_net.error("send_packet_from_p2p_port_ipv6: port %d not present", +SCE_NP_PORT);
			return false;
		}
	}

	return true;
}

std::vector<std::vector<u8>> get_rpcn_msgs()
{
	std::vector<std::vector<u8>> msgs;
	auto& nc = g_fxo->get<p2p_context>();
	{
		std::lock_guard list_lock(nc.list_p2p_ports_mutex);
		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);
			{
				std::lock_guard lock(def_port.s_rpcn_mutex);
				msgs = std::move(def_port.rpcn_msgs);
				def_port.rpcn_msgs.clear();
			}
		}
		else
		{
			sys_net.error("get_rpcn_msgs: port %d not present", +SCE_NP_PORT);
		}
	}

	return msgs;
}

std::vector<signaling_message> get_sign_msgs()
{
	std::vector<signaling_message> msgs;
	auto& nc = g_fxo->get<p2p_context>();
	{
		std::lock_guard list_lock(nc.list_p2p_ports_mutex);
		if (nc.list_p2p_ports.contains(SCE_NP_PORT))
		{
			auto& def_port = ::at32(nc.list_p2p_ports, SCE_NP_PORT);
			{
				std::lock_guard lock(def_port.s_sign_mutex);
				msgs = std::move(def_port.sign_msgs);
				def_port.sign_msgs.clear();
			}
		}
		else
		{
			sys_net.error("get_sign_msgs: port %d not present", +SCE_NP_PORT);
		}
	}

	return msgs;
}

namespace np
{
	void init_np_handler_dependencies();
}

void base_network_thread::add_ppu_to_awake(ppu_thread* ppu)
{
	std::lock_guard lock(mutex_ppu_to_awake);
	ppu_to_awake.emplace_back(ppu);
}

void base_network_thread::del_ppu_to_awake(ppu_thread* ppu)
{
	std::lock_guard lock(mutex_ppu_to_awake);

	for (auto it = ppu_to_awake.begin(); it != ppu_to_awake.end();)
	{
		if (*it == ppu)
		{
			it = ppu_to_awake.erase(it);
			continue;
		}

		it++;
	}
}

void base_network_thread::wake_threads()
{
	std::lock_guard lock(mutex_ppu_to_awake);

	ppu_to_awake.erase(std::unique(ppu_to_awake.begin(), ppu_to_awake.end()), ppu_to_awake.end());
	for (ppu_thread* ppu : ppu_to_awake)
	{
		network_clear_queue(*ppu);
		lv2_obj::append(ppu);
	}

	if (!ppu_to_awake.empty())
	{
		ppu_to_awake.clear();
		lv2_obj::awake_all();
	}
}

p2p_thread::p2p_thread()
{
	np::init_np_handler_dependencies();
}

void p2p_thread::bind_sce_np_port()
{
	std::lock_guard list_lock(list_p2p_ports_mutex);
	create_p2p_port(SCE_NP_PORT);
}

void network_thread::operator()()
{
	std::vector<shared_ptr<lv2_socket>> socklist;
	socklist.reserve(lv2_socket::id_count);

	{
		std::lock_guard lock(mutex_ppu_to_awake);
		ppu_to_awake.clear();
	}

	std::vector<::pollfd> fds(lv2_socket::id_count);
#ifdef _WIN32
	std::vector<bool> connecting(lv2_socket::id_count);
	std::vector<bool> was_connecting(lv2_socket::id_count);
#endif

	while (thread_ctrl::state() != thread_state::aborting)
	{
		if (!num_polls)
		{
			thread_ctrl::wait_on(num_polls, 0);
			continue;
		}

		ensure(socklist.size() <= lv2_socket::id_count);

		// Wait with 1ms timeout
#ifdef _WIN32
		windows_poll(fds, ::size32(socklist), 1, connecting);
#else
		::poll(fds.data(), socklist.size(), 1);
#endif

		std::lock_guard lock(mutex_thread_loop);

		for (usz i = 0; i < socklist.size(); i++)
		{
#ifdef _WIN32
			socklist[i]->handle_events(fds[i], was_connecting[i] && !connecting[i]);
#else
			socklist[i]->handle_events(fds[i]);
#endif
		}

		wake_threads();
		socklist.clear();

		// Obtain all native active sockets
		idm::select<lv2_socket>([&](u32 id, lv2_socket& s)
			{
				if (s.get_type() == SYS_NET_SOCK_DGRAM || s.get_type() == SYS_NET_SOCK_STREAM)
				{
					socklist.emplace_back(idm::get_unlocked<lv2_socket>(id));
				}
			});

		for (usz i = 0; i < socklist.size(); i++)
		{
			auto events = socklist[i]->get_events();

			fds[i].fd = events ? socklist[i]->get_socket() : -1;
			fds[i].events =
				(events & lv2_socket::poll_t::read ? POLLIN : 0) |
				(events & lv2_socket::poll_t::write ? POLLOUT : 0) |
				0;
			fds[i].revents = 0;
#ifdef _WIN32
			const auto cur_connecting = socklist[i]->is_connecting();
			was_connecting[i] = cur_connecting;
			connecting[i] = cur_connecting;
#endif
		}
	}
}

// Must be used under list_p2p_ports_mutex lock!
void p2p_thread::create_p2p_port(u16 p2p_port)
{
	if (!list_p2p_ports.contains(p2p_port))
	{
		list_p2p_ports.emplace(std::piecewise_construct, std::forward_as_tuple(p2p_port), std::forward_as_tuple(p2p_port));
		const u32 prev_value = num_p2p_ports.fetch_add(1);
		if (!prev_value)
		{
			num_p2p_ports.notify_one();
		}
	}
}

void p2p_thread::operator()()
{
	std::vector<::pollfd> p2p_fd(lv2_socket::id_count);

	while (thread_ctrl::state() != thread_state::aborting)
	{
		if (!num_p2p_ports)
		{
			thread_ctrl::wait_on(num_p2p_ports, 0);
			continue;
		}

		// Check P2P sockets for incoming packets
		auto num_p2p_sockets = 0;
		std::memset(p2p_fd.data(), 0, p2p_fd.size() * sizeof(::pollfd));
		{
			auto set_fd = [&](socket_type socket)
			{
				p2p_fd[num_p2p_sockets].events = POLLIN;
				p2p_fd[num_p2p_sockets].revents = 0;
				p2p_fd[num_p2p_sockets].fd = socket;
				num_p2p_sockets++;
			};

			std::lock_guard lock(list_p2p_ports_mutex);
			for (const auto& [_, p2p_port] : list_p2p_ports)
			{
				set_fd(p2p_port.p2p_socket);
			}
		}

#ifdef _WIN32
		// WSAPoll seems to consume a lot of CPU time relative to its waiting duration, upping the timeout solves it
		const auto ret_p2p = WSAPoll(p2p_fd.data(), num_p2p_sockets, 5);
#else
		const auto ret_p2p = ::poll(p2p_fd.data(), num_p2p_sockets, 1);
#endif
		if (ret_p2p > 0)
		{
			std::scoped_lock lock(mutex_thread_loop, list_p2p_ports_mutex);
			auto fd_index = 0;

			auto process_fd = [&](nt_p2p_port& p2p_port)
			{
				if ((p2p_fd[fd_index].revents & POLLIN) == POLLIN || (p2p_fd[fd_index].revents & POLLRDNORM) == POLLRDNORM)
				{
					while (p2p_port.recv_data())
						;
				}
				fd_index++;
			};

			for (auto& [_, p2p_port] : list_p2p_ports)
			{
				process_fd(p2p_port);
			}

			wake_threads();
		}
		else if (ret_p2p < 0)
		{
			sys_net.error("[P2P] Error poll on master P2P socket: %d", get_last_error(false));
		}
	}
}

// --------- ASBR CONNECTOR MODE: hello datagram sender (see psas_connector.h)

namespace
{
	// Hello targets: every valid "P2P Broadcast Forward" entry (same "ip:port,ip:port" rules as the broadcast
	// forward in lv2_socket_p2p::sendto), plus the connector on 127.0.0.1:4000 when connector mode is active
	// (it is a forward target then) or when nothing is configured (stock config, opt-out). Duplicates are dropped.
	std::vector<::sockaddr_in> get_psas_hello_targets(bool connector_mode, bool& forward_configured)
	{
		std::vector<::sockaddr_in> targets;

		const auto add_target = [&](u32 addr, u16 port)
		{
			for (const ::sockaddr_in& target : targets)
			{
				if (target.sin_addr.s_addr == addr && target.sin_port == port)
				{
					return;
				}
			}

			::sockaddr_in target{};
			target.sin_family = AF_INET;
			target.sin_addr.s_addr = addr;
			target.sin_port = port;
			targets.push_back(target);
		};

		const std::string cfg_fwd = g_cfg.net.p2p_broadcast_forward.to_string();

		for (usz start = 0; start < cfg_fwd.size();)
		{
			usz end = cfg_fwd.find(',', start);

			if (end == std::string::npos)
			{
				end = cfg_fwd.size();
			}

			std::string entry = cfg_fwd.substr(start, end - start);
			start = end + 1;

			while (!entry.empty() && entry.front() == ' ')
			{
				entry.erase(entry.begin());
			}

			while (!entry.empty() && entry.back() == ' ')
			{
				entry.pop_back();
			}

			const usz sep = entry.rfind(':');
			u16 fwd_port = 0;
			::in_addr fwd_ip{};

			const bool valid = sep != std::string::npos && sep != 0 && sep + 1 < entry.size() &&
				std::from_chars(entry.c_str() + sep + 1, entry.c_str() + entry.size(), fwd_port).ec == std::errc() &&
				inet_pton(AF_INET, entry.substr(0, sep).c_str(), &fwd_ip) == 1;

			if (valid)
			{
				add_target(fwd_ip.s_addr, std::bit_cast<u16, be_t<u16>>(fwd_port));
			}
		}

		forward_configured = !targets.empty();

		if (connector_mode || targets.empty())
		{
			add_target(std::bit_cast<u32, be_t<u32>>(0x7F000001), std::bit_cast<u16, be_t<u16>>(sys_net_helpers::PSAS_CONNECTOR_PORT));
		}

		return targets;
	}

	std::string psas_hello_target_to_string(const ::sockaddr_in& target)
	{
		const u16 port = std::bit_cast<be_t<u16>, u16>(target.sin_port);
		return fmt::format("%s:%d", np::ip_to_string(target.sin_addr.s_addr), port);
	}
} // namespace

psas_connector_hello_thread::psas_connector_hello_thread(std::string title_id)
	: m_title_id(std::move(title_id))
{
}

void psas_connector_hello_thread::operator()()
{
	// Both settings are fixed for the whole emulation run (not dynamic)
	const bool connector_mode = g_cfg.net.psas_connector_mode.get();
	bool forward_configured = false;
	const std::vector<::sockaddr_in> targets = get_psas_hello_targets(connector_mode, forward_configured);
	const std::string fork_version = sys_net_helpers::compose_psas_fork_version(rpcs3::get_version().to_string());

	const socket_type hello_socket = ::socket(AF_INET, SOCK_DGRAM, 0);

#ifdef _WIN32
	if (hello_socket == INVALID_SOCKET)
#else
	if (hello_socket == -1)
#endif
	{
		sys_net.error("ASBR connector mode: failed to create the hello socket (native error %d), the connector will only see the game traffic", get_native_error());
		return;
	}

	np::set_socket_non_blocking(hello_socket);

#ifdef _WIN32
	// Same as the P2P port: a hello sent while the connector is not running bounces with ICMP port-unreachable,
	// which Windows would otherwise report as WSAECONNRESET on the next sendto
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
	{
		BOOL new_behaviour   = FALSE;
		DWORD bytes_returned = 0;
		if (WSAIoctl(hello_socket, SIO_UDP_CONNRESET, &new_behaviour, sizeof(new_behaviour), nullptr, 0, &bytes_returned, nullptr, nullptr) != 0)
			sys_net.warning("ASBR connector mode: failed to disable SIO_UDP_CONNRESET on the hello socket (native error %d)", get_native_error());
	}
#endif

	bool logged_first = false;
	bool logged_send_error = false;

	while (thread_ctrl::state() != thread_state::aborting)
	{
		const u16 bound_port = sys_net_helpers::get_psas_connector_bound_port();

		u8 flags = sys_net_helpers::PSAS_HELLO_FLAG_TITLE_GATE;

		if (bound_port)
		{
			flags |= sys_net_helpers::PSAS_HELLO_FLAG_LOOPBACK_BIND;
		}

		if (connector_mode || forward_configured)
		{
			flags |= sys_net_helpers::PSAS_HELLO_FLAG_FORWARD;
		}

		if (!connector_mode)
		{
			flags |= sys_net_helpers::PSAS_HELLO_FLAG_OPT_OUT;
		}

		const std::string bound_addr = bound_port ? fmt::format("127.0.0.1:%d", bound_port) : std::string();

		const std::vector<u8> datagram = sys_net_helpers::encode_psas_connector_hello({
			.flags = flags,
			.interval_ms = sys_net_helpers::PSAS_HELLO_INTERVAL_MS,
			.fork_version = fork_version,
			.title_id = m_title_id,
			.bound_addr = bound_addr,
		});

		if (!std::exchange(logged_first, true))
		{
			// Self-check: the exact bytes of the first hello, to compare with the connector's log
			std::string targets_str;
			std::string datagram_hex;

			for (const ::sockaddr_in& target : targets)
			{
				fmt::append(targets_str, "%s%s", targets_str.empty() ? "" : ", ", psas_hello_target_to_string(target));
			}

			for (const u8 byte : datagram)
			{
				fmt::append(datagram_hex, "%s%02x", datagram_hex.empty() ? "" : " ", byte);
			}

			sys_net.notice("ASBR connector mode %s (%s): hello from '%s' every %d ms to %s, first datagram (%d bytes): %s",
				connector_mode ? "on" : "off (opt-out)", m_title_id, fork_version, sys_net_helpers::PSAS_HELLO_INTERVAL_MS, targets_str, datagram.size(), datagram_hex);
		}

		for (const ::sockaddr_in& target : targets)
		{
			if (::sendto(hello_socket, reinterpret_cast<const char*>(datagram.data()), ::size32(datagram), 0, reinterpret_cast<const ::sockaddr*>(&target), sizeof(target)) < 0)
			{
				const int native_error = get_native_error();

				if (!std::exchange(logged_send_error, true))
				{
					sys_net.error("ASBR connector mode: failed to send the hello datagram to %s (native error %d), further hello errors are not logged",
						psas_hello_target_to_string(target), native_error);
				}
			}
		}

		thread_ctrl::wait_for(sys_net_helpers::PSAS_HELLO_INTERVAL_MS * 1000ull);
	}

	np::close_socket(hello_socket);
}

void init_psas_connector_hello()
{
	if (!sys_net_helpers::is_psas_title_gate_matched())
	{
		return;
	}

	g_fxo->init<psas_connector_hello_context>(Emu.GetTitleID());
}
