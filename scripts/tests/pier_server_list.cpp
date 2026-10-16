// c++ -std=c++20 scripts/tests/pier_server_list.cpp -o <scratch>/server-list-test
#include "../../rpcs3/Emu/NP/server_list.h"
#include <array>
#include <cassert>
#include <iostream>

int main()
{
	std::array<std::uint16_t, 4> output{0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
	const std::array<std::uint16_t, 1> one{1};
	np::copy_server_ids(std::span{output}, one);
	assert((output == std::array<std::uint16_t, 4>{1, 0, 0, 0}));
	const std::array<std::uint16_t, 5> many{1, 2, 3, 4, 5};
	np::copy_server_ids(std::span{output}, many);
	assert((output == std::array<std::uint16_t, 4>{1, 2, 3, 4}));
	np::copy_server_ids(std::span{output}, {});
	assert((output == std::array<std::uint16_t, 4>{0, 0, 0, 0}));
	np::copy_server_ids(std::span<std::uint16_t>{}, one);
	// Capacity is bounded; memory after a short output span stays untouched.
	output = {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
	np::copy_server_ids(std::span{output}.first(2), one);
	assert((output == std::array<std::uint16_t, 4>{1, 0, 0xcccc, 0xdddd}));
	std::cout << "server list regression checks passed\n";
}
