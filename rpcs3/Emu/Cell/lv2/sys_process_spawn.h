#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pier
{
	struct spawn_arguments
	{
		std::vector<std::string> argv;
		std::vector<std::string> envp;
	};

	// Firmware FUN_00608950 stores BE u64 pointers followed by two terminated
	// pointer lists and their strings, all contained in the supplied guest block.
	inline std::optional<spawn_arguments> decode_spawn_arguments(
		std::span<const std::uint8_t> block, std::uint32_t guest_base)
	{
		if (block.size() < 24 || block.size() > 0x10000 ||
			std::uint64_t{guest_base} + block.size() > 0x100000000ull)
			return std::nullopt;
		spawn_arguments result;
		std::size_t offset = 0;
		for (auto* list : {&result.argv, &result.envp})
		{
			for (;;)
			{
				if (offset + 8 > block.size())
					return std::nullopt;
				std::uint64_t address = 0;
				for (unsigned i = 0; i < 8; ++i)
					address = (address << 8) | block[offset + i];
				offset += 8;
				if (!address)
					break;
				if (address < guest_base || address - guest_base >= block.size())
					return std::nullopt;
				const auto position = static_cast<std::size_t>(address - guest_base);
				const char* text = reinterpret_cast<const char*>(block.data() + position);
				const char* nul = static_cast<const char*>(std::memchr(text, 0, block.size() - position));
				if (!nul)
					return std::nullopt;
				list->emplace_back(text, nul);
			}
		}
		if (result.argv.empty())
			return std::nullopt;
		return result;
	}

	// Exact guest argv/envp for a native child host, as hex of NUL-terminated
	// strings. The CLI "--" form would insert an extra empty argument.
	inline std::string encode_spawn_list(const std::vector<std::string>& list)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string text;
		for (const auto& entry : list)
			for (const unsigned char c : entry + '\0')
				text += {digits[c >> 4], digits[c & 15]};
		return text;
	}

	inline std::optional<std::vector<std::string>> decode_spawn_list(std::string_view text)
	{
		auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
		if (text.size() % 2)
			return std::nullopt;
		std::vector<std::string> list;
		std::string current;
		for (std::size_t i = 0; i < text.size(); i += 2)
		{
			const int high = nibble(text[i]), low = nibble(text[i + 1]);
			if (high < 0 || low < 0)
				return std::nullopt;
			if (const char c = static_cast<char>(high << 4 | low))
				current += c;
			else
				list.push_back(std::exchange(current, {}));
		}
		if (!current.empty())
			return std::nullopt;
		return list;
	}
} // namespace pier
