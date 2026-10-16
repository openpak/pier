// Linux: compile with -std=c++20 -pthread; executable and scratch belong outside /tmp.
#include "../../rpcs3/Emu/Cell/lv2/native_console_sync.h"

#include <atomic>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <vector>

extern char** environ;
using memory = pier::native_console_memory;
using console_sync = pier::native_console_sync;
using status = console_sync::status;
using mode = console_sync::open_mode;
using protocol = console_sync::protocol;

namespace
{
[[noreturn]] void error(const char* message, int value)
{
	throw std::system_error(value, std::generic_category(), message);
}
void check(bool value)
{
	if (!value) { std::cerr << "native synchronization regression failed\n"; std::abort(); }
}
template <typename Predicate> void until(Predicate&& predicate)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate())
	{
		check(std::chrono::steady_clock::now() < end);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
auto never_cancel = [] { return false; };
constexpr auto test_key = memory::sysutil_key + 0x10;
constexpr auto dead_key = memory::sysutil_key + 0x11;
constexpr auto stress_key = memory::sysutil_key + 0x12;
constexpr auto condition_key = memory::sysutil_key + 0x20;

pid_t launch(const char* executable, const char* task, int fd)
{
	posix_spawn_file_actions_t actions;
	check(::posix_spawn_file_actions_init(&actions) == 0);
	check(::posix_spawn_file_actions_adddup2(&actions, fd, 198) == 0);
	char inherited[] = "198";
	char* args[] = {const_cast<char*>(executable), const_cast<char*>(task), inherited, nullptr};
	pid_t child;
	check(::posix_spawn(&child, executable, &actions, nullptr, args, environ) == 0);
	check(::posix_spawn_file_actions_destroy(&actions) == 0);
	return child;
}
void joined(pid_t child)
{
	int result;
	check(::waitpid(child, &result, 0) == child);
	check(WIFEXITED(result) && WEXITSTATUS(result) == 0);
}
}

int main(int argc, char** argv)
{
	if (argc == 3)
	{
		const int fd = std::stoi(argv[2]);
		auto console = memory::attach(fd, &error);
		::close(fd);
		const std::string task{argv[1]};
		if (task == "--poison")
		{
			auto mapping = console.map_control();
			auto& control = *reinterpret_cast<pier::native_console_control*>(mapping.bytes().data());
			check(::pthread_mutex_lock(&control.lock) == 0);
			::_exit(0); // Simulate death in a metadata transaction.
		}
		console_sync kernel{console, &error};
		auto shared = console.map(memory::sysutil_key);
		auto* phase = reinterpret_cast<std::uint32_t*>(shared.bytes().data());
		if (task == "--child")
		{
			check(kernel.open_mutex(test_key, mode::create, protocol::priority, false).code == status::exists);
			auto opened = kernel.open_mutex(test_key, mode::open, protocol::priority, false);
			check(opened.code == status::ok);
			check(opened.handle->try_lock(1) == status::busy); // Same guest TID, different host.
			check(opened.handle->unlock(1) == status::not_owner);
			check(opened.handle->lock(1, 1000, 10'000, never_cancel) == status::timed_out);
			__atomic_store_n(phase, 1u, __ATOMIC_RELEASE);
			check(opened.handle->lock(1, 1000, 2'000'000, never_cancel) == status::ok);
			shared.bytes()[0x400] = std::byte{0x7f};
			check(opened.handle->unlock(1) == status::ok);
			return 0;
		}
		if (task == "--die")
		{
			auto opened = kernel.open_mutex(dead_key, mode::open, protocol::fifo, false);
			check(opened.code == status::ok);
			check(opened.handle->try_lock(1) == status::ok);
			__atomic_store_n(phase, 4u, __ATOMIC_RELEASE);
			::_exit(0); // Skip handle/session destructors while owning a guest lock.
		}
		if (task == "--condition")
		{
			auto mutex = kernel.open_mutex(test_key, mode::open, protocol::priority, false);
			check(mutex.code == status::ok);
			auto condition = kernel.open_condition(condition_key, mode::open, *mutex.handle);
			check(condition.code == status::ok);
			check(mutex.handle->try_lock(1) == status::ok);
			__atomic_store_n(phase, 3u, __ATOMIC_RELEASE);
			check(condition.handle->wait(1, 100, 2'000'000, never_cancel) == status::ok);
			check(shared.bytes()[0x401] == std::byte{0x6e});
			check(mutex.handle->try_lock(1) == status::deadlock);
			check(mutex.handle->unlock(1) == status::ok);
			return 0;
		}
		if (task == "--stress")
		{
			auto opened = kernel.open_mutex(stress_key, mode::open, protocol::fifo, false);
			check(opened.code == status::ok);
			for (int count = 0; count < 2000; ++count)
			{
				check(opened.handle->lock(1, 1000, 2'000'000, never_cancel) == status::ok);
				++*phase;
				check(opened.handle->unlock(1) == status::ok);
			}
			return 0;
		}
		return 1;
	}

	auto console = memory::create(&error);
	console_sync kernel{console, &error};
	check(kernel.open_mutex(test_key, mode::open, protocol::fifo, false).code == status::not_found);
	check(kernel.open_mutex(0, mode::create, protocol::fifo, false).code == status::invalid);
	check(kernel.open_mutex(test_key, mode::create, protocol::priority_inherit, false).code == status::unsupported);
	auto main = kernel.open_mutex(test_key, mode::create, protocol::priority, false);
	check(main.code == status::ok);
	check(main.handle->try_lock(0) == status::invalid);
	check(main.handle->try_lock(1) == status::ok);
	check(main.handle->try_lock(1) == status::deadlock);
	check(main.handle->unlock(2) == status::not_owner);
	auto shared = console.map(memory::sysutil_key);
	auto* phase = reinterpret_cast<std::uint32_t*>(shared.bytes().data());
	const auto child = launch(argv[0], "--child", console.fd());
	until([&] { return __atomic_load_n(phase, __ATOMIC_ACQUIRE) == 1; });
	check(main.handle->unlock(1) == status::ok);
	joined(child);
	check(shared.bytes()[0x400] == std::byte{0x7f});
	check(main.handle->idle() == status::ok);
	auto condition = kernel.open_condition(condition_key, mode::create, *main.handle);
	check(condition.code == status::ok);
	check(kernel.open_condition(condition_key, mode::create, *main.handle).code == status::exists);
	check(condition.handle->wait(1, 100, 1000, never_cancel) == status::not_owner);
	check(main.handle->idle() == status::has_conditions);
	*phase = 0;
	const auto native_waiter = launch(argv[0], "--condition", console.fd());
	until([&] { return __atomic_load_n(phase, __ATOMIC_ACQUIRE) == 3; });
	check(main.handle->lock(1, 100, 2'000'000, never_cancel) == status::ok);
	check(condition.handle->waiter_count() == 1);
	check(condition.handle->idle() == status::busy);
	shared.bytes()[0x401] = std::byte{0x6e};
	check(condition.handle->signal() == status::ok);
	check(condition.handle->waiter_count() == 0);
	check(main.handle->waiter_count() == 1);
	check(main.handle->unlock(1) == status::ok);
	joined(native_waiter);
	condition.handle.reset();
	check(main.handle->idle() == status::ok);
	check(kernel.open_condition(condition_key, mode::open, *main.handle).code == status::not_found);
	const auto forked = ::fork();
	check(forked >= 0);
	if (!forked)
	{
		check(main.handle->try_lock(1) == status::invalid);
		check(kernel.open_mutex(test_key, mode::open, protocol::fifo, false).code == status::invalid);
		return 0; // Inherited destructors must not unregister the parent peer.
	}
	joined(forked);
	check(main.handle->try_lock(1) == status::ok);
	check(main.handle->unlock(1) == status::ok);

	// Queue a lower-priority waiter first. Priority handoff must select the
	// later higher-priority waiter; FIFO must preserve arrival order instead.
	for (const auto ordering : {protocol::priority, protocol::fifo})
	{
		auto opened = kernel.open_mutex(0x123 + static_cast<int>(ordering), mode::create, ordering, false);
		check(opened.code == status::ok);
		auto& mutex = *opened.handle;
		check(mutex.try_lock(10) == status::ok);
		std::vector<int> order;
		auto wait = [&](std::uint32_t thread, int priority)
		{
			check(mutex.lock(thread, priority, 2'000'000, never_cancel) == status::ok);
			order.push_back(thread);
			check(mutex.unlock(thread) == status::ok);
		};
		std::thread low{wait, 11, 1000};
		until([&] { return mutex.waiter_count() == 1; });
		std::thread high{wait, 12, 100};
		until([&] { return mutex.waiter_count() == 2; });
		check(mutex.unlock(10) == status::ok);
		low.join(); high.join();
		check(order == (ordering == protocol::priority ? std::vector<int>{12, 11} : std::vector<int>{11, 12}));
	}

	check(main.handle->try_lock(1) == status::ok);
	std::atomic<bool> cancel = false;
	status cancelled = status::ok;
	std::thread waiting{[&] { cancelled = main.handle->lock(2, 100, 0, [&] { return cancel.load(); }); }};
	until([&] { return main.handle->waiter_count() == 1; });
	cancel = true;
	waiting.join();
	check(cancelled == status::cancelled && main.handle->waiter_count() == 0);
	check(main.handle->try_lock(2) == status::busy);
	check(main.handle->unlock(1) == status::ok);
	check(main.handle->try_lock(2) == status::ok);
	check(main.handle->unlock(2) == status::ok);

	auto recursive = kernel.open_mutex(0x456, mode::open_or_create, protocol::fifo, true);
	check(recursive.code == status::ok);
	check(recursive.handle->try_lock(3) == status::ok);
	check(recursive.handle->try_lock(3) == status::ok);
	check(recursive.handle->unlock(3) == status::ok);
	check(recursive.handle->try_lock(4) == status::busy);
	check(recursive.handle->unlock(3) == status::ok);
	check(recursive.handle->try_lock(4) == status::ok);
	check(recursive.handle->unlock(4) == status::ok);
	auto recursive_condition = kernel.open_condition(0x789, mode::create, *recursive.handle);
	check(recursive_condition.code == status::ok);
	check(recursive.handle->try_lock(3) == status::ok);
	check(recursive.handle->try_lock(3) == status::ok);
	check(recursive_condition.handle->wait(3, 100, 10'000, never_cancel) == status::timed_out);
	check(recursive.handle->unlock(3) == status::ok);
	check(recursive.handle->try_lock(4) == status::busy);
	check(recursive.handle->unlock(3) == status::ok);
	check(recursive.handle->try_lock(4) == status::ok);
	check(recursive.handle->unlock(4) == status::ok);

	// A timed-out condition waiter must reacquire the mutex before returning.
	auto timed_mutex = kernel.open_mutex(0x900, mode::create, protocol::fifo, false);
	check(timed_mutex.code == status::ok);
	auto timed_condition = kernel.open_condition(0x901, mode::create, *timed_mutex.handle);
	check(timed_condition.code == status::ok);
	std::atomic<bool> returned = false;
	std::thread timeout_waiter{[&]
	{
		check(timed_mutex.handle->try_lock(71) == status::ok);
		check(timed_condition.handle->wait(71, 100, 20'000, never_cancel) == status::timed_out);
		returned = true;
		check(timed_mutex.handle->unlock(71) == status::ok);
	}};
	until([&] { return timed_condition.handle->waiter_count() == 1; });
	check(timed_mutex.handle->lock(72, 100, 100'000, never_cancel) == status::ok);
	until([&] { return timed_mutex.handle->waiter_count() == 1; });
	check(!returned);
	check(timed_mutex.handle->unlock(72) == status::ok);
	timeout_waiter.join();
	check(returned);

	for (const auto ordering : {protocol::priority, protocol::fifo})
	{
		auto mutex = kernel.open_mutex(0xa00 + static_cast<int>(ordering), mode::create, ordering, false);
		check(mutex.code == status::ok);
		auto event = kernel.open_condition(0xb00 + static_cast<int>(ordering), mode::create, *mutex.handle);
		check(event.code == status::ok);
		std::vector<int> order;
		auto wait = [&](std::uint32_t thread, int priority)
		{
			check(mutex.handle->lock(thread, priority, 2'000'000, never_cancel) == status::ok);
			check(event.handle->wait(thread, priority, 2'000'000, never_cancel) == status::ok);
			order.push_back(thread);
			check(mutex.handle->unlock(thread) == status::ok);
		};
		std::thread low{wait, 11, 1000};
		until([&] { return event.handle->waiter_count() == 1; });
		std::thread high{wait, 12, 100};
		until([&] { return event.handle->waiter_count() == 2; });
		check(mutex.handle->try_lock(13) == status::ok);
		check(event.handle->signal() == status::ok);
		check(event.handle->signal(true) == status::ok);
		check(mutex.handle->unlock(13) == status::ok);
		low.join(); high.join();
		check(order == (ordering == protocol::priority ? std::vector<int>{12, 11} : std::vector<int>{11, 12}));
	}

	cancel = false;
	std::thread condition_cancel{[&]
	{
		check(timed_mutex.handle->try_lock(81) == status::ok);
		check(timed_condition.handle->wait(81, 100, 0, [&] { return cancel.load(); }) == status::cancelled);
	}};
	until([&] { return timed_condition.handle->waiter_count() == 1; });
	cancel = true;
	condition_cancel.join();
	check(timed_mutex.handle->try_lock(82) == status::ok);
	check(timed_mutex.handle->unlock(82) == status::ok);
	check(timed_condition.handle->idle() == status::ok);

	auto stress = kernel.open_mutex(stress_key, mode::create, protocol::fifo, false);
	check(stress.code == status::ok);
	*phase = 0;
	const auto first = launch(argv[0], "--stress", console.fd());
	const auto second = launch(argv[0], "--stress", console.fd());
	joined(first); joined(second);
	check(*phase == 4000);

	auto dead = kernel.open_mutex(dead_key, mode::create, protocol::fifo, false);
	check(dead.code == status::ok);
	*phase = 0;
	const auto dying = launch(argv[0], "--die", console.fd());
	// Leave the child as a zombie until after owner-death detection. The
	// registry must not depend on a supervisor having reaped it already.
	until([&] { return __atomic_load_n(phase, __ATOMIC_ACQUIRE) == 4; });
	check(dead.handle->lock(1, 1000, 500'000, never_cancel) == status::owner_dead);
	joined(dying);
	check(dead.handle->unlock(1) == status::owner_dead);
	check(kernel.open_mutex(dead_key, mode::create, protocol::fifo, false).code == status::exists);
	dead.handle.reset();
	auto fresh = kernel.open_mutex(dead_key, mode::create, protocol::fifo, false);
	check(fresh.code == status::ok && fresh.handle->try_lock(1) == status::ok);
	check(fresh.handle->unlock(1) == status::ok);

	auto isolated = memory::create(&error);
	console_sync other{isolated, &error};
	check(other.open_condition(condition_key, mode::create, *main.handle).code == status::invalid);
	check(other.open_mutex(test_key, mode::open, protocol::fifo, false).code == status::not_found);
	std::vector<console_sync::open_result> full;
	for (std::uint64_t key = 0x10000;; ++key)
	{
		auto entry = other.open_mutex(key, mode::create, protocol::fifo, false);
		if (entry.code == status::no_space) break;
		check(entry.code == status::ok);
		full.push_back(std::move(entry));
	}
	check(full.size() == console_sync::max_mutexes);
	check(other.open_mutex(0x10000, mode::open, protocol::fifo, false).code == status::ok);
	full.clear();
	check(other.open_mutex(0x10000, mode::open, protocol::fifo, false).code == status::not_found);
	check(other.open_mutex(0x10000, mode::create, protocol::fifo, false).code == status::ok);

	// Robust metadata lock loss fails the whole domain, never grants a lock.
	joined(launch(argv[0], "--poison", isolated.fd()));
	check(other.open_mutex(7, mode::create, protocol::fifo, false).code == status::broken);
	std::cout << "PASS: cross-process native mutex/condition ownership, FIFO/priority, timeout/reacquire, cancellation, recursion, broadcast, contention, owner death, isolation, capacity and metadata-death rejection\n";
}
