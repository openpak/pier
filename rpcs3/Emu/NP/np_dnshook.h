#pragma once

#include <unordered_map>
#include <queue>
#include <vector>
#include <optional>

#include "util/types.hpp"
#include "Utilities/mutex.h"

namespace np
{
	class dnshook
	{
	public:
		dnshook();

		void add_dns_spy(u32 sock);
		void remove_dns_spy(u32 sock);

		bool is_dns(u32 sock);
		bool is_dns_queue(u32 sock);

		std::vector<u8> get_dns_packet(u32 sock);
		s32 analyze_dns_packet(s32 s, const u8* buf, u32 len);

	private:
		std::optional<u32> get_redir(const std::string& hostname);

	private:
		shared_mutex mutex;
		std::unordered_map<s32, std::queue<std::vector<u8>>> m_dns_spylist;
		std::vector<std::pair<std::string, u32>> m_redirs;

		// Pier: OpenPak DNS catcher (ps3-patcher v0.2.17 parity). When PIER_DNS_CATCHER names a list
		// file, the signed-list matcher in Emu/NP/openpak_catcher.h decides each lookup: a matching
		// name is answered with the box address here, every other name is passed to the configured
		// DNS server untouched (so plain upstream DNS resolves the owner's own network). Caught names
		// are logged, passed names only counted.
		bool m_catcher = false;
		u32 m_catcher_passed = 0;
	};
} // namespace np
