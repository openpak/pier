// lv2 UDPP2P framing: golden headers from the 4.93 kernel's sender and receiver.
#include "../../rpcs3/Emu/Cell/lv2/sys_net/udpp2p.h"
#include <cstdlib>
#include <vector>
void check(bool result) { if (!result) std::abort(); }
int main()
{
	const std::uint8_t body[] = {0xaa, 0xbb};
	// VSH's signaling socket: 3658 vport 0xfffe on both ends, so the 2-byte form.
	auto big = udpp2p::frame(0xfffe, 0xfffe, body, 2);
	check(big == std::vector<std::uint8_t>{0xff, 0x83, 0xff, 0xfe, 0xff, 0xfe, 0xaa, 0xbb});
	auto h = udpp2p::parse(big.data(), big.size());
	check(h && h->kind == 3 && h->src_vport == 0xfffe && h->dst_vport == 0xfffe && h->size == 6);
	// Both vports below 0x100: compact, source first.
	auto small = udpp2p::frame(1, 2, body, 2);
	check(small == std::vector<std::uint8_t>{0xff, 0xc3, 0x01, 0x02, 0xaa, 0xbb});
	h = udpp2p::parse(small.data(), small.size());
	check(h && h->src_vport == 1 && h->dst_vport == 2 && h->size == 4);
	// A keyed kind parses (so it can be dropped), an unreachable reply has no vports.
	const std::uint8_t keyed[] = {0xff, 0xc4, 1, 1}, unreach[] = {0xff, 0x87};
	check(udpp2p::parse(keyed, 4)->kind == 4);
	check(udpp2p::parse(unreach, 2)->kind == 7);
	// RPCS3's own framing (little-endian vports) and truncated headers are not UDPP2P.
	const std::uint8_t rpcs3[] = {0x01, 0x00, 0x01, 0x00, 0x01, 0x00}, cut[] = {0xff, 0x83, 0xff};
	check(!udpp2p::parse(rpcs3, 6));
	check(!udpp2p::parse(cut, 3));
	check(!udpp2p::parse(small.data(), 1));
}
