#pragma once

#include <cstdint>
#include <span>

namespace np
{
	// Clear unused capacity as well: some titles scan every supplied slot and use
	// zero to identify an absent server, independently of the returned count.
	template <typename T, std::size_t Extent>
	void copy_server_ids(std::span<T, Extent> output, std::span<const std::uint16_t> servers)
	{
		for (std::size_t i = 0; i < output.size(); ++i)
			output[i] = i < servers.size() ? servers[i] : 0;
	}
} // namespace np
