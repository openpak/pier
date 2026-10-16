// Standalone regression: c++ -std=c++20 scripts/tests/pier_spawn_arguments.cpp -o <scratch>/spawn-test
#include "../../rpcs3/Emu/Cell/lv2/sys_process_spawn.h"
#include <cassert>
#include <algorithm>
#include <iostream>

int main()
{
	constexpr std::uint32_t base = 0x10020000;
	auto put = [](auto& block, std::size_t offset, std::uint64_t value)
	{
		for (int i = 7; i >= 0; --i)
		{
			block[offset + i] = value & 255;
			value >>= 8;
		}
	};
	std::vector<std::uint8_t> block(192);
	const std::string path = "/dev_hdd0/game/BLES01938/USRDIR/EBOOT.BIN";
	std::copy(path.begin(), path.end(), block.begin() + 64);
	const std::string arg = "--example";
	std::copy(arg.begin(), arg.end(), block.begin() + 112);
	const std::string env = "LANG=en";
	std::copy(env.begin(), env.end(), block.begin() + 144);
	put(block, 0, base + 64);
	put(block, 8, base + 112);
	put(block, 24, base + 144); // argv NULL at16, envp NULL at32
	auto result = pier::decode_spawn_arguments(block, base);
	assert(result && result->argv == std::vector<std::string>({path, arg}));
	assert(result->envp == std::vector<std::string>({env}));
	// The observed XMB HDD launch uses an 0x50-byte block, one path and no env.
	std::vector<std::uint8_t> xmb(0x50);
	std::copy(path.begin(), path.end(), xmb.begin() + 32);
	put(xmb, 0, base + 32);
	result = pier::decode_spawn_arguments(xmb, base);
	assert(result && result->argv == std::vector<std::string>({path}) && result->envp.empty());
	auto bad = block;
	put(bad, 0, std::uint64_t{base} + block.size());
	assert(!pier::decode_spawn_arguments(bad, base));
	bad = block;
	put(bad, 0, base - 1);
	assert(!pier::decode_spawn_arguments(bad, base));
	bad = block;
	put(bad, 0, 0x100000000ull + base);
	assert(!pier::decode_spawn_arguments(bad, base));
	bad = block;
	std::fill(bad.begin() + 64, bad.end(), 'x');
	assert(!pier::decode_spawn_arguments(bad, base));
	assert(!pier::decode_spawn_arguments(std::span(block).first(23), base));
	assert(!pier::decode_spawn_arguments(block, 0xfffffff0));
	bad.assign(24, 0);
	put(bad, 0, base + 23);
	put(bad, 8, base + 23);
	put(bad, 16, base + 23);
	assert(!pier::decode_spawn_arguments(bad, base)); // no argv terminator
	bad.assign(24, 0);
	assert(!pier::decode_spawn_arguments(bad, base)); // empty argv
	const std::vector<std::string> list{path, "", "\xff spaced=\x01"};
	assert(pier::decode_spawn_list(pier::encode_spawn_list(list)) == list);
	assert(pier::decode_spawn_list("")->empty());
	assert(!pier::decode_spawn_list("41")); // unterminated
	assert(!pier::decode_spawn_list("4100f")); // odd length
	assert(!pier::decode_spawn_list("4G00")); // invalid digit
	std::cout << "spawn argument regression checks passed\n";
}
