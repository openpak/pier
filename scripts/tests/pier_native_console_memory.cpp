// Linux regression: c++ -std=c++20 -Wall -Wextra -Werror this.cpp -o <scratch>/test
#include "../../rpcs3/Emu/Cell/lv2/native_console_sync.h"

#include <cassert>
#include <chrono>
#include <csignal>
#include <iostream>
#include <spawn.h>
#include <string>
#include <system_error>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace
{
int private_state = 42;

[[noreturn]] void raise_error(const char* operation, int error)
{
	throw std::system_error(error, std::generic_category(), operation);
}

template <typename F>
void rejects(F&& operation)
{
	bool rejected = false;
	try { operation(); }
	catch (const std::exception&) { rejected = true; }
	assert(rejected);
}
}

int main(int argc, char** argv)
{
	using memory = pier::native_console_memory;
	if (argc >= 3 && (std::string(argv[1]) == "--observe" || std::string(argv[1]) == "--observe-sync" || std::string(argv[1]) == "--observe-events"))
	{
		const bool observe_events = std::string(argv[1]) == "--observe-events";
		const bool observe_sync = std::string(argv[1]) != "--observe";
		auto console = memory::create(&raise_error);
		std::optional<pier::native_console_sync> kernel;
		if (observe_sync) kernel.emplace(console, &raise_error);
		auto transport = console.map(memory::sysutil_key);
		std::vector<std::string> environment;
		for (char** entry = environ; *entry; ++entry)
		{
			if (!std::string_view(*entry).starts_with("PIER_NATIVE_CONSOLE_FD="))
				environment.emplace_back(*entry);
		}
		environment.emplace_back("PIER_NATIVE_CONSOLE_FD=198");
		std::vector<char*> env;
		for (auto& entry : environment)
			env.push_back(entry.data());
		env.push_back(nullptr);

		posix_spawn_file_actions_t actions;
		assert(::posix_spawn_file_actions_init(&actions) == 0);
		assert(::posix_spawn_file_actions_adddup2(&actions, console.fd(), 198) == 0);
		pid_t child;
		assert(::posix_spawn(&child, argv[2], &actions, nullptr, argv + 2, env.data()) == 0);
		assert(::posix_spawn_file_actions_destroy(&actions) == 0);
		bool observed = false;
		int status;
		bool running = true;
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(150);
		while (std::chrono::steady_clock::now() < end)
		{
			// Observe firmware-written headers. These bytes start at zero; this
			// helper never constructs or writes a packet header on its behalf.
			const volatile std::byte* data = transport.bytes().data();
			auto header_ready = [&](std::size_t offset)
			{
				return data[offset] == std::byte{0} && data[offset + 1] == std::byte{0} &&
					data[offset + 2] == std::byte{4} && data[offset + 3] == std::byte{0} &&
					data[offset + 4] == std::byte{0} && data[offset + 5] == std::byte{0} &&
					data[offset + 6] == std::byte{0x7c} && data[offset + 7] == std::byte{0};
			};
			if (header_ready(0) && header_ready(0x8000))
			{
				bool ready = true;
				std::size_t native_waiters = 0;
				if (kernel)
				{
					using sync_type = pier::native_console_sync;
					for (std::uint64_t channel = 0; channel < 2 && ready; ++channel)
					{
						auto mutex = kernel->open_mutex(0x8006010000000020ULL + channel, sync_type::open_mode::open, sync_type::protocol::priority, false);
						if (mutex.code != sync_type::status::ok) { ready = false; break; }
						for (std::uint64_t suffix : {0x30, 0x40, 0x50, 0x60})
						{
							auto condition = kernel->open_condition(0x8006010000000000ULL + suffix + channel, sync_type::open_mode::open, *mutex.handle);
							if (condition.code != sync_type::status::ok) { ready = false; break; }
							native_waiters += condition.handle->waiter_count();
						}
						if (ready)
						{
							ready = mutex.handle->lock(1, 1000, 100'000, [] { return false; }) == sync_type::status::ok;
							if (ready)
							{
								ready = header_ready(channel * 0x8000);
								ready = mutex.handle->unlock(1) == sync_type::status::ok && ready;
							}
						}
					}
				}
				if (ready && observe_events)
				{
					auto queue = kernel->open_event(0x8006040000000400ULL, pier::native_console_sync::open_mode::open);
					ready = queue.code == pier::native_console_sync::status::ok && queue.handle->alive() == pier::native_console_sync::status::ok;
					const auto firmware = kernel->inspect_guest(1);
					const auto main_thread = kernel->resolve_ppu_thread(0x01000000);
					ready = ready && firmware.code == pier::native_console_sync::status::ok && firmware.info.parent == 0 &&
						firmware.info.state == pier::native_console_sync::guest_state::running &&
						main_thread.code == pier::native_console_sync::status::ok;
				}
				if (ready)
				{
					observed = true;
					if (kernel) std::cout << "Native firmware condition waiters observed: " << native_waiters << '\n';
					break;
				}
			}
			if (::waitpid(child, &status, WNOHANG) == child)
			{
				running = false;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (running)
		{
			::kill(child, SIGTERM);
			const auto stop_end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (::waitpid(child, &status, WNOHANG) == 0)
			{
				if (std::chrono::steady_clock::now() >= stop_end)
				{
					::kill(child, SIGKILL);
					assert(::waitpid(child, &status, 0) == child);
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
		std::cout << (observed ? "PASS" : "FAIL") << ": native firmware packet headers visible in supervisor's shared system memory\n";
		if (observe_sync && observed) std::cout << "PASS: supervisor opened firmware-created native mutexes/conditions and acquired both channel locks\n";
		if (observe_events && observed) std::cout << "PASS: supervisor opened live firmware-created keyed event queue and resolved real guest PID/PPU identity (no events submitted)\n";
		return observed ? 0 : 1;
	}

	if (argc == 3 && std::string(argv[1]) == "--child")
	{
		assert(private_state == 42);
		private_state = 99;
		const int inherited = std::stoi(argv[2]);
		auto console = memory::attach(inherited, &raise_error);
		assert(::fcntl(console.fd(), F_GETFD) & FD_CLOEXEC);
		::close(inherited);
		auto transport = console.map(memory::sysutil_key);
		assert(transport.bytes()[0] == std::byte{0x42});
		assert(transport.bytes().back() == std::byte{0x24});
		transport.bytes()[0x400] = std::byte{0x99};
		auto mio = console.map(memory::mio_key);
		assert(mio.bytes()[0] == std::byte{0x11});
		mio.bytes().back() = std::byte{0x22};
		return 0;
	}

	auto console = memory::create(&raise_error);
	assert(::fcntl(console.fd(), F_GETFD) & FD_CLOEXEC);
	auto transport = console.map(memory::sysutil_key);
	assert(transport.bytes()[0] == std::byte{});
	transport.bytes()[0] = std::byte{0x42};
	transport.bytes().back() = std::byte{0x24};
	auto mio = console.map(memory::mio_key);
	mio.bytes()[0] = std::byte{0x11};

	// Exec a separate host process. Only the console descriptor is passed;
	// ordinary process memory must remain independent.
	posix_spawn_file_actions_t actions;
	assert(::posix_spawn_file_actions_init(&actions) == 0);
	constexpr int inherited = 198;
	assert(console.fd() != inherited);
	assert(::posix_spawn_file_actions_adddup2(&actions, console.fd(), inherited) == 0);
	char fd_arg[] = "198";
	char child_arg[] = "--child";
	char* child_argv[] = {argv[0], child_arg, fd_arg, nullptr};
	pid_t child;
	assert(::posix_spawn(&child, argv[0], &actions, nullptr, child_argv, environ) == 0);
	assert(::posix_spawn_file_actions_destroy(&actions) == 0);
	int status;
	assert(::waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(private_state == 42);
	assert(transport.bytes()[0x400] == std::byte{0x99});
	assert(mio.bytes().back() == std::byte{0x22});
	assert(transport.bytes().back() == std::byte{0x24});

	// A second console has the same native keys, with independent storage.
	auto isolated = memory::create(&raise_error);
	auto other_transport = isolated.map(memory::sysutil_key);
	assert(other_transport.bytes()[0] == std::byte{});
	assert(other_transport.bytes()[0x400] == std::byte{});
	rejects([&] { console.map(0x1234); });
	rejects([] { memory::attach(-1, &raise_error); });

	// Attaching never silently creates an arena for malformed input.
	const int malformed = static_cast<int>(::syscall(SYS_memfd_create, "invalid-console-test", MFD_CLOEXEC | MFD_ALLOW_SEALING));
	assert(malformed >= 0);
	assert(::ftruncate(malformed, memory::storage_size) == 0);
	rejects([&] { memory::attach(malformed, &raise_error); }); // unsealed
	assert(::fcntl(malformed, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
	rejects([&] { memory::attach(malformed, &raise_error); }); // wrong header
	::close(malformed);
	assert(::ftruncate(console.fd(), 0) == -1 && errno == EPERM);
	assert(::ftruncate(console.fd(), memory::storage_size * 2) == -1 && errno == EPERM);

	// A mapping keeps storage alive after its owning descriptor is closed.
	auto retained = []
	{
		auto owner = memory::create(&raise_error);
		auto view = owner.map(memory::sysutil_key);
		view.bytes()[7] = std::byte{7};
		return view;
	}();
	assert(retained.bytes()[7] == std::byte{7});
	std::cout << "PASS: exec-separated shared system memory, console isolation, descriptor lifetime, invalid attachment and resize rejection\n";
}
