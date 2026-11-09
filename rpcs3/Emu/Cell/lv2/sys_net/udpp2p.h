#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

// Real lv2 UDPP2P framing (firmware 4.93 kernel: send FUN_0015fd68, receive FUN_0016063c).
// Every DGRAM_P2P datagram on UDP 3658 starts with 0xff, then 0x80 | kind (| 0x40 when both
// vports fit a byte), then the source vport and the destination vport, one byte each when
// compact, else big-endian u16 each. Kind 3 is plain data; 4-6 are the keyed (signed or
// encrypted) variants and 7-10 the kernel's unreachable replies (no vports).
namespace udpp2p
{
	constexpr std::uint8_t kind_plain = 3;

	struct header
	{
		std::uint8_t kind;
		std::uint16_t src_vport;
		std::uint16_t dst_vport;
		std::size_t size;
	};

	inline std::vector<std::uint8_t> frame(std::uint16_t src_vport, std::uint16_t dst_vport, const std::uint8_t* data, std::size_t size)
	{
		const bool compact = src_vport < 0x100 && dst_vport < 0x100;
		const std::size_t head = compact ? 4 : 6;
		std::vector<std::uint8_t> out(head + size);
		out[0] = 0xff;
		out[1] = 0x80 | (compact ? 0x40 : 0) | kind_plain;
		if (compact)
		{
			out[2] = static_cast<std::uint8_t>(src_vport);
			out[3] = static_cast<std::uint8_t>(dst_vport);
		}
		else
		{
			out[2] = src_vport >> 8, out[3] = src_vport & 0xff;
			out[4] = dst_vport >> 8, out[5] = dst_vport & 0xff;
		}
		if (size)
			std::memcpy(out.data() + head, data, size);
		return out;
	}

	// Empty when the datagram is not lv2 UDPP2P (RPCS3's own P2PS/vport-0 framing).
	inline std::optional<header> parse(const std::uint8_t* data, std::size_t size)
	{
		// ponytail: the kernel also accepts bit 7 clear with an explicit u16 length; lv2's own
		// sender always sets bit 7, so that form is treated as foreign framing.
		if (size < 2 || data[0] != 0xff || !(data[1] & 0x80))
			return std::nullopt;

		const std::uint8_t kind = data[1] & 0xf;
		if (kind >= 7 && kind <= 10)
			return header{kind, 0, 0, size};
		if (kind < 3 || kind > 6)
			return std::nullopt;

		if (data[1] & 0x40)
		{
			if (size < 4)
				return std::nullopt;
			return header{kind, data[2], data[3], 4};
		}

		if (size < 6)
			return std::nullopt;
		return header{kind, static_cast<std::uint16_t>(data[2] << 8 | data[3]), static_cast<std::uint16_t>(data[4] << 8 | data[5]), 6};
	}
} // namespace udpp2p
