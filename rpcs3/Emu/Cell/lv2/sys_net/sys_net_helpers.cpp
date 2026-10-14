#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Emu/Cell/PPUThread.h"
#include "lv2_socket.h"
#include "sys_net_helpers.h"
#include "network_context.h"

#include "Emu/NP/np_handler.h"
#include "Emu/NP/np_helpers.h"
#include "Emu/Cell/Modules/cellNetCtl.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <unistd.h>
#endif

#include <cstdio>

LOG_CHANNEL(sys_net);

int get_native_error()
{
	int native_error;
#ifdef _WIN32
	native_error = WSAGetLastError();
#else
	native_error = errno;
#endif

	return native_error;
}

sys_net_error convert_error(bool is_blocking, int native_error, [[maybe_unused]] bool is_connecting)
{
	// Convert the error code for socket functions to a one for sys_net
	sys_net_error result{};
	const char* name{};

#ifdef _WIN32
#define ERROR_CASE(error)         \
	case WSA##error:              \
		result = SYS_NET_##error; \
		name = #error;            \
		break;
#else
#define ERROR_CASE(error)         \
	case error:                   \
		result = SYS_NET_##error; \
		name = #error;            \
		break;
#endif
	switch (native_error)
	{
#ifndef _WIN32
		ERROR_CASE(ENOENT);
		ERROR_CASE(ENOMEM);
		ERROR_CASE(EBUSY);
		ERROR_CASE(ENOSPC);
		ERROR_CASE(EPIPE);
#endif

		// TODO: We don't currently support EFAULT or EINTR
		// ERROR_CASE(EFAULT);
		// ERROR_CASE(EINTR);

		ERROR_CASE(EBADF);
		ERROR_CASE(EACCES);
		ERROR_CASE(EINVAL);
		ERROR_CASE(EMFILE);
		ERROR_CASE(EWOULDBLOCK);
		ERROR_CASE(EINPROGRESS);
		ERROR_CASE(EALREADY);
		ERROR_CASE(EDESTADDRREQ);
		ERROR_CASE(EMSGSIZE);
		ERROR_CASE(EPROTOTYPE);
		ERROR_CASE(ENOPROTOOPT);
		ERROR_CASE(EPROTONOSUPPORT);
		ERROR_CASE(EOPNOTSUPP);
		ERROR_CASE(EPFNOSUPPORT);
		ERROR_CASE(EAFNOSUPPORT);
		ERROR_CASE(EADDRINUSE);
		ERROR_CASE(EADDRNOTAVAIL);
		ERROR_CASE(ENETDOWN);
		ERROR_CASE(ENETUNREACH);
		ERROR_CASE(ECONNABORTED);
		ERROR_CASE(ECONNRESET);
		ERROR_CASE(ENOBUFS);
		ERROR_CASE(EISCONN);
		ERROR_CASE(ENOTCONN);
		ERROR_CASE(ESHUTDOWN);
		ERROR_CASE(ETOOMANYREFS);
		ERROR_CASE(ETIMEDOUT);
		ERROR_CASE(ECONNREFUSED);
		ERROR_CASE(EHOSTDOWN);
		ERROR_CASE(EHOSTUNREACH);
#ifdef _WIN32
	// Windows likes to be special with unique errors
	case WSAENETRESET:
		result = SYS_NET_ECONNRESET;
		name = "WSAENETRESET";
		break;
#endif
	default:
		fmt::throw_exception("sys_net get_last_error(is_blocking=%d, native_error=%d): Unknown/illegal socket error", is_blocking, native_error);
	}

#ifdef _WIN32
	if (is_connecting)
	{
		// Windows will return SYS_NET_ENOTCONN when recvfrom/sendto is called on a socket that is connecting but not yet connected
		if (result == SYS_NET_ENOTCONN)
			return SYS_NET_EAGAIN;
	}
#endif

	if (name && result != SYS_NET_EWOULDBLOCK && result != SYS_NET_EINPROGRESS)
	{
		sys_net.error("Socket error %s", name);
	}

	if (is_blocking && result == SYS_NET_EWOULDBLOCK)
	{
		return {};
	}

	if (is_blocking && result == SYS_NET_EINPROGRESS)
	{
		return {};
	}

	return result;
#undef ERROR_CASE
}

sys_net_error get_last_error(bool is_blocking, bool is_connecting)
{
	return convert_error(is_blocking, get_native_error(), is_connecting);
}

sys_net_sockaddr native_addr_to_sys_net_addr(const ::sockaddr_storage& native_addr)
{
	ensure(native_addr.ss_family == AF_INET || native_addr.ss_family == AF_UNSPEC);

	sys_net_sockaddr sn_addr;

	sys_net_sockaddr_in* paddr = reinterpret_cast<sys_net_sockaddr_in*>(&sn_addr);
	*paddr = {};

	paddr->sin_len = sizeof(sys_net_sockaddr_in);
	paddr->sin_family = SYS_NET_AF_INET;
	paddr->sin_port = std::bit_cast<be_t<u16>, u16>(reinterpret_cast<const sockaddr_in*>(&native_addr)->sin_port);
	paddr->sin_addr = std::bit_cast<be_t<u32>, u32>(reinterpret_cast<const sockaddr_in*>(&native_addr)->sin_addr.s_addr);

	return sn_addr;
}

::sockaddr_in sys_net_addr_to_native_addr(const sys_net_sockaddr& sn_addr)
{
	ensure(sn_addr.sa_family == SYS_NET_AF_INET);

	const sys_net_sockaddr_in* psa_in = reinterpret_cast<const sys_net_sockaddr_in*>(&sn_addr);

	::sockaddr_in native_addr{};
	native_addr.sin_family = AF_INET;
	native_addr.sin_port = std::bit_cast<u16>(psa_in->sin_port);
	native_addr.sin_addr.s_addr = std::bit_cast<u32>(psa_in->sin_addr);

#ifdef _WIN32
	// Windows doesn't support sending packets to 0.0.0.0 but it works on unixes, send to 127.0.0.1 instead
	if (native_addr.sin_addr.s_addr == 0x00000000)
	{
		auto& nph = g_fxo->get<named_thread<np::np_handler>>();
		if (const u32 bind_addr = nph.get_bind_ip(); bind_addr != 0)
		{
			// If bind IP is set 0.0.0.0 was bound to binding_ip so we need to connect to that ip
			sys_net.warning("[Native] Redirected 0.0.0.0 to %s", np::ip_to_string(bind_addr));
			native_addr.sin_addr.s_addr = bind_addr;
		}
		else
		{
			// Otherwise we connect to localhost which should be bound
			sys_net.warning("[Native] Redirected 0.0.0.0 to 127.0.0.1");
			native_addr.sin_addr.s_addr = std::bit_cast<u32, be_t<u32>>(0x7F000001);
		}
	}
#endif

	return native_addr;
}

bool is_ip_public_address(const ::sockaddr_in& addr)
{
	const u8* ip = reinterpret_cast<const u8*>(&addr.sin_addr.s_addr);

	if ((ip[0] == 10) ||
		(ip[0] == 127) ||
		(ip[0] == 172 && (ip[1] >= 16 && ip[1] <= 31)) ||
		(ip[0] == 192 && ip[1] == 168))
	{
		return false;
	}

	return true;
}

u32 network_clear_queue(ppu_thread& ppu)
{
	u32 cleared = 0;

	idm::select<lv2_socket>([&](u32, lv2_socket& sock)
		{
			cleared += sock.clear_queue(&ppu);
		});

	return cleared;
}

void clear_ppu_to_awake(ppu_thread& ppu)
{
	g_fxo->get<network_context>().del_ppu_to_awake(&ppu);
	g_fxo->get<p2p_context>().del_ppu_to_awake(&ppu);
}

be_t<u32> resolve_binding_ip()
{
	in_addr conv{};
	const std::string cfg_bind_addr = g_cfg.net.bind_address.to_string();

	if (cfg_bind_addr == "0.0.0.0" || cfg_bind_addr == "")
	{
		return 0;
	}

	if (!inet_pton(AF_INET, cfg_bind_addr.c_str(), &conv))
	{
		// Do not set to disconnected on invalid IP just error and continue using default (0.0.0.0)
		sys_net.error("Provided IP(%s) address for bind is invalid!", g_cfg.net.bind_address.to_string());
		return 0;
	}

	return conv.s_addr;
}

#ifdef _WIN32
// Workaround function for WSAPoll not reporting failed connections
// Note that this was fixed in Windows 10 version 2004 (after more than 10 years lol)
void windows_poll(std::vector<pollfd>& fds, unsigned long nfds, int timeout, std::vector<bool>& connecting)
{
	ensure(fds.size() >= nfds);
	ensure(connecting.size() >= nfds);

	// Don't call WSAPoll with zero nfds (errors 10022 or 10038)
	if (std::none_of(fds.begin(), fds.begin() + nfds, [](pollfd& pfd)
			{
				return pfd.fd != INVALID_SOCKET;
			}))
	{
		if (timeout > 0)
		{
			Sleep(timeout);
		}

		return;
	}

	int r = ::WSAPoll(fds.data(), nfds, timeout);

	if (r == SOCKET_ERROR)
	{
		sys_net.error("WSAPoll failed: %s", fmt::win_error{static_cast<unsigned long>(WSAGetLastError()), nullptr});
		return;
	}

	for (unsigned long i = 0; i < nfds; i++)
	{
		if (connecting[i])
		{
			if (!fds[i].revents)
			{
				int error = 0;
				socklen_t intlen = sizeof(error);
				if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &intlen) == -1 || error != 0)
				{
					// Connection silently failed
					connecting[i] = false;
					fds[i].revents = POLLERR | POLLHUP | (fds[i].events & (POLLIN | POLLOUT));
				}
			}
			else
			{
				connecting[i] = false;
			}
		}
	}
}
#endif

namespace
{
	// Netmask and default gateway of the host interface that owns `ip`. Network byte order, 0 = unknown.
	void net_host_ipv4_params(u32 ip, u32& mask, u32& gateway)
	{
		mask = 0;
		gateway = 0;

#ifdef _WIN32
		ULONG size = 0;
		if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW)
		{
			return;
		}

		std::vector<u8> buf(size);
		auto* info = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());

		if (GetAdaptersInfo(info, &size) != NO_ERROR)
		{
			return;
		}

		for (auto* adapter = info; adapter; adapter = adapter->Next)
		{
			for (auto* entry = &adapter->IpAddressList; entry; entry = entry->Next)
			{
				in_addr addr{}, netmask{}, gw{};

				if (inet_pton(AF_INET, entry->IpAddress.String, &addr) == 1 && addr.s_addr == ip && inet_pton(AF_INET, entry->IpMask.String, &netmask) == 1)
				{
					mask = netmask.s_addr;

					if (inet_pton(AF_INET, adapter->GatewayList.IpAddress.String, &gw) == 1)
					{
						gateway = gw.s_addr;
					}

					return;
				}
			}
		}
#else
		[[maybe_unused]] std::string ifname;
		ifaddrs* ifap = nullptr;

		if (getifaddrs(&ifap) != 0)
		{
			return;
		}

		for (const ifaddrs* p = ifap; p; p = p->ifa_next)
		{
			if (p->ifa_addr && p->ifa_netmask && p->ifa_addr->sa_family == AF_INET && reinterpret_cast<const ::sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr == ip)
			{
				mask = reinterpret_cast<const ::sockaddr_in*>(p->ifa_netmask)->sin_addr.s_addr;
				ifname = p->ifa_name;
				break;
			}
		}

		freeifaddrs(ifap);

#ifdef __linux__
		// Default route of that interface: Destination 0, Mask 0, RTF_GATEWAY (0x2). Fields are printed as raw
		// network-order words, so sscanf gives the same value as in_addr::s_addr.
		if (!ifname.empty())
		{
			if (FILE* f = std::fopen("/proc/net/route", "r"))
			{
				char line[256];

				while (std::fgets(line, sizeof(line), f))
				{
					char iface[64]{};
					unsigned dest = 0, gw = 0, flags = 0, refcnt = 0, use = 0, metric = 0, msk = 0;

					if (std::sscanf(line, "%63s %x %x %x %u %u %u %x", iface, &dest, &gw, &flags, &refcnt, &use, &metric, &msk) == 8 && ifname == iface && dest == 0 && msk == 0 && (flags & 0x2) && gw)
					{
						gateway = gw;
						break;
					}
				}

				std::fclose(f);
			}
		}
#endif
#endif
	}
}

bool net_get_lease(net_lease_t& lease)
{
	auto& nph = g_fxo->get<named_thread<np::np_handler>>();

	// No host route (np_handler stays Disconnected): the console is offline, nothing to offer
	if (nph.get_net_status() != CELL_NET_CTL_STATE_IPObtained || !nph.get_local_ip_addr())
	{
		return false;
	}

	lease.ip = nph.get_local_ip_addr();
	lease.dns = nph.get_dns_ip();
	lease.mac = nph.get_ether_addr();

	u32 mask = 0, gateway = 0;
	net_host_ipv4_params(lease.ip, mask, gateway);

	// Host netmask unreadable (sandbox, exotic OS): assume /24
	if (!mask)
	{
		mask = std::bit_cast<u32>(std::array<u8, 4>{255, 255, 255, 0});
	}

	// Host default route unreadable, or not on this subnet: x.y.z.1 of the subnet (x.y.z.254 if that is the console itself)
	if (!gateway || ((gateway ^ lease.ip) & mask))
	{
		gateway = (lease.ip & mask) | std::bit_cast<u32>(std::array<u8, 4>{0, 0, 0, 1});

		if (gateway == lease.ip)
		{
			gateway = (lease.ip & mask) | std::bit_cast<u32>(std::array<u8, 4>{0, 0, 0, 254});
		}
	}

	lease.mask = mask;
	lease.gateway = gateway;
	return true;
}

void net_dhcp_serve(socket_type guest_socket, const std::vector<u8>& req)
{
	// BOOTP header (236 bytes) + magic cookie + at least one option
	if (req.size() < 241 || req[0] != 1 || req[236] != 0x63 || req[237] != 0x82 || req[238] != 0x53 || req[239] != 0x63)
	{
		return;
	}

	u8 msg_type = 0;

	for (usz i = 240; i + 1 < req.size() && req[i] != 255;)
	{
		if (req[i] == 0)
		{
			i++;
			continue;
		}

		const usz len = req[i + 1];

		if (i + 2 + len > req.size())
		{
			break;
		}

		if (req[i] == 53 && len >= 1)
		{
			msg_type = req[i + 2];
		}

		i += 2 + len;
	}

	// DISCOVER -> OFFER, REQUEST -> ACK. Everything else (DECLINE, RELEASE, INFORM) is dropped.
	if (msg_type != 1 && msg_type != 3)
	{
		sys_net.notice("net_dhcp_serve: DHCP message type %d ignored", msg_type);
		return;
	}

	net_lease_t lease;

	if (!net_get_lease(lease))
	{
		sys_net.notice("net_dhcp_serve: host has no route, no lease offered");
		return;
	}

	sys_net.notice("net_dhcp_serve: DHCP%s xid=0x%08x -> %s: ip=%s mask=%s gateway=%s dns=%s", msg_type == 1 ? "DISCOVER" : "REQUEST", (u32{req[4]} << 24 | u32{req[5]} << 16 | u32{req[6]} << 8 | u32{req[7]}), msg_type == 1 ? "OFFER" : "ACK", np::ip_to_string(lease.ip), np::ip_to_string(lease.mask), np::ip_to_string(lease.gateway), np::ip_to_string(lease.dns));

	constexpr u32 lease_secs = 86400;
	std::vector<u8> r(300, 0);

	r[0] = 2;                                    // BOOTREPLY
	r[1] = req[1];                               // htype
	r[2] = req[2];                               // hlen
	std::memcpy(&r[4], &req[4], 4);              // xid, the firmware matches it
	std::memcpy(&r[10], &req[10], 2);            // flags
	if (msg_type == 3)
	{
		std::memcpy(&r[12], &req[12], 4);        // ciaddr (renewals)
	}
	std::memcpy(&r[16], &lease.ip, 4);           // yiaddr
	std::memcpy(&r[20], &lease.gateway, 4);      // siaddr
	std::memcpy(&r[28], &req[28], 16);           // chaddr, the firmware compares the first 6 bytes with its own MAC
	r[236] = 0x63; r[237] = 0x82; r[238] = 0x53; r[239] = 0x63;

	usz pos = 240;

	const auto put = [&](u8 code, const void* data, u8 len)
	{
		r[pos++] = code;
		r[pos++] = len;
		std::memcpy(&r[pos], data, len);
		pos += len;
	};

	const auto put_u32 = [&](u8 code, u32 v)
	{
		const be_t<u32> be = v;
		put(code, &be, 4);
	};

	const u8 reply_type = msg_type == 1 ? 2 : 5;
	put(53, &reply_type, 1);
	put(54, &lease.gateway, 4);       // server id: required, non-zero, by the firmware's ACK handling
	put_u32(51, lease_secs);          // lease time: required, non-zero
	put_u32(58, lease_secs / 2);      // T1
	put_u32(59, lease_secs / 8 * 7);  // T2
	put(1, &lease.mask, 4);
	put(3, &lease.gateway, 4);

	if (lease.dns)
	{
		put(6, &lease.dns, 4);
	}

	const u32 broadcast = lease.ip | ~lease.mask;
	put(28, &broadcast, 4);
	r[pos++] = 255;

	// The guest socket sits on an ephemeral host port (see lv2_socket_native::bind). Hand the reply to it from a
	// throw-away host socket: the guest's recvfrom/select/poll then work unchanged.
	::sockaddr_in dst{};
	::socklen_t dst_len = sizeof(dst);

	if (::getsockname(guest_socket, reinterpret_cast<::sockaddr*>(&dst), &dst_len) != 0 || dst.sin_family != AF_INET)
	{
		return;
	}

	if (dst.sin_addr.s_addr == 0)
	{
		dst.sin_addr.s_addr = std::bit_cast<u32>(std::array<u8, 4>{127, 0, 0, 1});
	}

	const auto s = ::socket(AF_INET, SOCK_DGRAM, 0);

#ifdef _WIN32
	if (s == INVALID_SOCKET)
#else
	if (s < 0)
#endif
	{
		return;
	}

	if (::sendto(s, reinterpret_cast<const char*>(r.data()), static_cast<int>(r.size()), 0, reinterpret_cast<const ::sockaddr*>(&dst), sizeof(dst)) < 0)
	{
		sys_net.error("net_dhcp_serve: delivering the reply failed (%d)", get_native_error());
	}
	else
	{
		sys_net.notice("net_dhcp_serve: reply delivered to %s:%d", np::ip_to_string(dst.sin_addr.s_addr), std::bit_cast<be_t<u16>, u16>(dst.sin_port));
	}

#ifdef _WIN32
	::closesocket(s);
#else
	::close(s);
#endif
}
