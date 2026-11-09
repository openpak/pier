#pragma once

#include <set>

#include "lv2_socket_p2ps.h"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#else
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

// dst_vport src_vport flags
constexpr s32 VPORT_P2P_HEADER_SIZE = sizeof(u16) + sizeof(u16) + sizeof(u16);

enum VPORT_P2P_FLAGS
{
	P2P_FLAG_P2P  = 1,
	P2P_FLAG_P2PS = 1 << 1,
};

#include "udpp2p.h"

namespace udpp2p
{
	// Sends one datagram from host P2P port `port`; in a native game child that port is
	// VSH's, so the packet goes out through VSH's socket (see nt_p2p_port::child_relay).
	s32 send(socket_type s, u16 port, const std::vector<u8>& packet, const ::sockaddr_in& dst, int native_flags);
} // namespace udpp2p

struct signaling_message
{
	u32 src_addr = 0;
	u16 src_port = 0;

	std::vector<u8> data;
};

namespace sys_net_helpers
{
	bool all_reusable(const std::set<s32>& sock_ids);
}

struct nt_p2p_port
{
	// Real socket where P2P packets are received/sent
	socket_type p2p_socket = 0;
	u16 port               = 0;

	shared_mutex bound_p2p_vports_mutex;
	// For DGRAM_P2P sockets (vport, sock_ids)
	std::map<u16, std::set<s32>> bound_p2p_vports{};
	// For STREAM_P2P sockets (vport, sock_ids)
	std::map<u16, std::set<s32>> bound_p2ps_vports{};
	// List of active(either from a connect or an accept) P2PS sockets (key, sock_id)
	// key is ( (src_vport) << 48 | (dst_vport) << 32 | addr ) with src_vport and addr being 0 for listening sockets
	std::map<u64, s32> bound_p2p_streams{};
	// Current free port index
	u16 binding_port = 30000;

	// Queued messages from RPCN
	shared_mutex s_rpcn_mutex;
	std::vector<std::vector<u8>> rpcn_msgs{};
	// Queued signaling messages
	shared_mutex s_sign_mutex;
	std::vector<signaling_message> sign_msgs{};

	std::array<u8, 65535> p2p_recv_data{};

	// Native console (Linux): VSH owns the host's UDP port and a game child shares it through
	// a loopback relay, so guest P2P packets leave from 3658 as on one real PS3. On VSH's side
	// this is the child's loopback address, learned from the child's first relayed packet.
	std::optional<::sockaddr_storage> child_relay;

	// Hands a received DGRAM_P2P payload to the sockets bound to `dst_vport`.
	bool deliver_dgram(u16 dst_vport, u16 src_vport, const ::sockaddr_storage& from, std::vector<u8> data);

	nt_p2p_port(u16 port);
	~nt_p2p_port();

	static void dump_packet(p2ps_encapsulated_tcp* tcph);

	u16 get_port();

	bool handle_connected(s32 sock_id, p2ps_encapsulated_tcp* tcp_header, u8* data, ::sockaddr_storage* op_addr);
	bool handle_listening(s32 sock_id, p2ps_encapsulated_tcp* tcp_header, u8* data, ::sockaddr_storage* op_addr);
	bool recv_data();
};
