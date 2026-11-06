#include "stdafx.h"
#include "Emu/NP/np_dnshook.h"

#include "Emu/system_config.h"
#include "Utilities/StrUtil.h"
#include "util/logs.hpp"

#include <regex>
#include <cstdlib>
#include <cstdio>
#include <fstream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include "Emu/NP/openpak_catcher.h"
#pragma GCC diagnostic pop

#ifdef _WIN32
#include <WS2tcpip.h>
#else
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <sys/socket.h>
#include <arpa/inet.h>
#ifdef __clang__
#pragma GCC diagnostic pop
#endif
#endif

LOG_CHANNEL(dnshook_log, "DnsHook");
LOG_CHANNEL(catcher_log, "DnsCatcher");

static catcher_list s_catch_list{};

namespace np
{
	dnshook::dnshook()
	{
		// Pier: load the OpenPak catcher list (PIER_DNS_CATCHER) instead of the RPCN swap list.
		if (const char* path = std::getenv("PIER_DNS_CATCHER"))
		{
			std::ifstream f(path);
			std::string kind, value;
			catcher_list l{};
			while (f >> kind >> value)
			{
				if (kind == "ip")
				{
					unsigned a, b, c, d;
					if (std::sscanf(value.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
					{
						l.ip[0] = static_cast<u8>(a); l.ip[1] = static_cast<u8>(b);
						l.ip[2] = static_cast<u8>(c); l.ip[3] = static_cast<u8>(d);
					}
				}
				else
				{
					const int k = kind == "family" ? CATCHER_FAMILY : kind == "exact" ? CATCHER_EXACT : kind == "never" ? CATCHER_NEVER : -1;
					if (k < 0 || !catcher_add(&l, k, value.c_str(), static_cast<int>(value.size())))
						catcher_log.error("list entry refused: %s %s", kind, value);
				}
			}
			if (l.n && l.ip[0])
			{
				s_catch_list = l;
				m_catcher = true;
				catcher_log.success("catcher on: %d families, answer %u.%u.%u.%u (every other name goes to the configured DNS)", l.n, l.ip[0], l.ip[1], l.ip[2], l.ip[3]);
				for (int i = 0; i < l.n; i++)
					catcher_log.notice("entry %d: %s %s", i, l.kind[i] == CATCHER_FAMILY ? "family" : l.kind[i] == CATCHER_EXACT ? "exact" : "never", l.name[i]);
				return;
			}
			catcher_log.error("list %s missing or empty: catcher off, using the swap list", path);
		}
		// Init switch map for dns
		const std::string swap_list = g_cfg.net.swap_list.to_string();
		const auto swaps = fmt::split_sv(swap_list, {"&&"});
		for (usz i = 0; i < swaps.size(); i++)
		{
			auto host_and_ip = fmt::split(swaps[i], {"="});
			if (host_and_ip.size() != 2)
			{
				dnshook_log.error("Pattern <%s> contains none or more than one '='", swaps[i]);
				continue;
			}

			in_addr conv;
			if (!inet_pton(AF_INET, host_and_ip[1].c_str(), &conv))
			{
				dnshook_log.error("IP(%s) provided for %s in the switch list is invalid!", host_and_ip[1], host_and_ip[0]);
			}
			else
			{
				m_redirs.push_back({std::move(host_and_ip[0]), conv.s_addr});
			}
		}
	}

	void dnshook::add_dns_spy(u32 sock)
	{
		std::lock_guard lock(mutex);
		m_dns_spylist.emplace(std::make_pair(sock, std::queue<std::vector<u8>>()));
	}

	void dnshook::remove_dns_spy(u32 sock)
	{
		std::lock_guard lock(mutex);
		m_dns_spylist.erase(sock);
	}

	bool dnshook::is_dns(u32 sock)
	{
		std::lock_guard lock(mutex);
		return m_dns_spylist.contains(sock);
	}

	bool dnshook::is_dns_queue(u32 sock)
	{
		std::lock_guard lock(mutex);
		return !::at32(m_dns_spylist, sock).empty();
	}

	std::vector<u8> dnshook::get_dns_packet(u32 sock)
	{
		std::lock_guard lock(mutex);
		auto ret_vec = std::move(::at32(m_dns_spylist, sock).front());
		::at32(m_dns_spylist, sock).pop();

		return ret_vec;
	}

	std::optional<u32> dnshook::get_redir(const std::string& hostname)
	{
		if (m_catcher)
		{
			const int hit = catcher_match(&s_catch_list, hostname.c_str());
			if (hit < 0)
			{
				m_catcher_passed++;
				catcher_log.warning("passed %s to the configured DNS (%u passed so far)", hostname, m_catcher_passed);
				return std::nullopt;
			}
			u32 ip;
			std::memcpy(&ip, s_catch_list.ip, 4);
			catcher_log.success("caught %s (family %s) -> %u.%u.%u.%u", hostname, s_catch_list.name[hit], s_catch_list.ip[0], s_catch_list.ip[1], s_catch_list.ip[2], s_catch_list.ip[3]);
			return ip;
		}

		for (const auto& [pattern, ip] : m_redirs)
		{
			const std::regex regex_pattern(fmt::replace_all(pattern, {{".", "\\."}, {"*", ".*"}}), std::regex_constants::icase);

			if (std::regex_match(hostname, regex_pattern))
			{
				dnshook_log.trace("Matched '%s' to pattern '%s'", hostname, pattern);
				return ip;
			}
		}

		return std::nullopt;
	}

	s32 dnshook::analyze_dns_packet(s32 s, const u8* buf, u32 len)
	{
		std::lock_guard lock(mutex);

		dnshook_log.trace("DNS REQUEST:\n%s", fmt::buf_to_hexstring(buf, len));

		struct dns_header
		{
			u16 id; // identification number

			u8 rd : 1;     // recursion desired
			u8 tc : 1;     // truncated message
			u8 aa : 1;     // authoritive answer
			u8 opcode : 4; // purpose of message
			u8 qr : 1;     // query/response flag

			u8 rcode : 4; // response code
			u8 cd : 1;    // checking disabled
			u8 ad : 1;    // authenticated data
			u8 z : 1;     // its z! reserved
			u8 ra : 1;    // recursion available

			be_t<u16> q_count;    // number of question entries
			be_t<u16> ans_count;  // number of answer entries
			be_t<u16> auth_count; // number of authority entries
			be_t<u16> add_count;  // number of resource entries
		};

		if (len < sizeof(dns_header))
			return -1;

		const dns_header* dhead = reinterpret_cast<const dns_header*>(buf);
		// We are only looking for queries not truncated(todo?), only handle one dns query at a time(todo?)
		if (dhead->qr != 0 || dhead->tc != 0 || dhead->q_count != 1 || dhead->ans_count != 0 || dhead->auth_count != 0 || dhead->add_count != 0)
			return -1;

		// Get the actual address
		u8 count = 0;
		std::string host{};
		for (u32 i = sizeof(dns_header); (i < len) && buf[i] != 0; i++)
		{
			if (count == 0)
			{
				count = buf[i];
				if (i != sizeof(dns_header))
				{
					host += '.';
				}
			}
			else
			{
				host += static_cast<char>(buf[i]);
				count--;
			}
		}

		dnshook_log.warning("DNS query for %s", host);

		if (auto ip = get_redir(host))
		{
			// design fake packet
			std::vector<u8> fake(len);
			memcpy(fake.data(), buf, len);
			dns_header* fake_header = reinterpret_cast<dns_header*>(fake.data());
			fake_header->qr         = 1;
			fake_header->ra         = 1;
			fake_header->ans_count  = 1;
			fake.insert(fake.end(), {0xC0, 0x0C});             // Ref to name in header
			fake.insert(fake.end(), {0x00, 0x01});             // IPv4
			fake.insert(fake.end(), {0x00, 0x01});             // Class?
			fake.insert(fake.end(), {0x00, 0x00, 0x00, 0x3B}); // TTL
			fake.insert(fake.end(), {0x00, 0x04});             // Size of data
			u8* ptr_ip = reinterpret_cast<u8*>(&(*ip));
			fake.insert(fake.end(), ptr_ip, ptr_ip + 4); // IP

			dnshook_log.warning("Solving %s to %d.%d.%d.%d", host, ptr_ip[0], ptr_ip[1], ptr_ip[2], ptr_ip[3]);

			m_dns_spylist[s].push(std::move(fake));
			return len;
		}
		return -1;
	}
} // namespace np
