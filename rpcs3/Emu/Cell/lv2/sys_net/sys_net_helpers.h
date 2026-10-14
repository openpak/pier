#pragma once

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
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

#include "Emu/Cell/lv2/sys_net.h"
#include "Emu/NP/ip_address.h"

int get_native_error();
sys_net_error convert_error(bool is_blocking, int native_error, bool is_connecting = false);
sys_net_error get_last_error(bool is_blocking, bool is_connecting = false);
sys_net_sockaddr native_addr_to_sys_net_addr(const ::sockaddr_storage& native_addr);
::sockaddr_in sys_net_addr_to_native_addr(const sys_net_sockaddr& sn_addr);
bool is_ip_public_address(const ::sockaddr_in& addr);
u32 network_clear_queue(ppu_thread& ppu);
void clear_ppu_to_awake(ppu_thread& ppu);
be_t<u32> resolve_binding_ip();

// Pier: the lease the emulated DHCP server of the console's wired port (eth0) hands out.
// Everything comes from the host (np_handler's discovered IP, the host interface netmask and default route,
// g_cfg.net.dns). All addresses are in network byte order (same memory layout as in_addr::s_addr).
struct net_lease_t
{
	u32 ip = 0;
	u32 mask = 0;
	u32 gateway = 0;
	u32 dns = 0;
	std::array<u8, 6> mac{};
};

// Returns false when the host has no route: the console is then offline and no lease is offered
bool net_get_lease(net_lease_t& lease);

// Answer a DHCPDISCOVER/DHCPREQUEST the guest sent to port 67 by delivering the reply to the guest's own
// UDP socket (bound to 0.0.0.0:68, hosted on an ephemeral host port) over the host loopback
void net_dhcp_serve(socket_type guest_socket, const std::vector<u8>& request);

#ifdef _WIN32
void windows_poll(std::vector<pollfd>& fds, unsigned long nfds, int timeout, std::vector<bool>& connecting);
#endif
