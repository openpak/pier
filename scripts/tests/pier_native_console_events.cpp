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
using kernel = pier::native_console_sync;
using status = kernel::status;
using mode = kernel::open_mode;
using protocol = kernel::protocol;
[[noreturn]] void error(const char* message, int value) { throw std::system_error(value, std::generic_category(), message); }
void check(bool result) { if (!result) std::abort(); }
auto never = [] { return false; };
template <typename Predicate> void until(Predicate predicate)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate()) { check(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
pid_t launch(const char* executable, const char* task, int fd)
{
	posix_spawn_file_actions_t actions;
	check(!posix_spawn_file_actions_init(&actions));
	check(!posix_spawn_file_actions_adddup2(&actions, fd, 198));
	char* args[] = {const_cast<char*>(executable), const_cast<char*>(task), nullptr};
	pid_t child;
	check(!posix_spawn(&child, executable, &actions, nullptr, args, environ));
	check(!posix_spawn_file_actions_destroy(&actions));
	return child;
}
void join(pid_t child) { int result; check(waitpid(child, &result, 0) == child); check(WIFEXITED(result) && !WEXITSTATUS(result)); }
int main(int argc, char** argv)
{
	if (argc == 2)
	{
		auto console = memory::attach(198, error);
		close(198);
		kernel k{console, error};
		if (std::string_view(argv[1]) == "--creator-dies")
		{
			auto q = k.open_event(200, mode::create);
			check(q.code == status::ok);
			auto phase = console.map(memory::sysutil_key);
			__atomic_store_n(reinterpret_cast<unsigned*>(phase.bytes().data()), 1u, __ATOMIC_RELEASE);
			while (__atomic_load_n(reinterpret_cast<unsigned*>(phase.bytes().data()), __ATOMIC_ACQUIRE) != 2) std::this_thread::yield();
			_exit(0);
		}
		auto q = k.open_event(100, mode::open);
		check(q.code == status::ok);
		if (std::string_view(argv[1]) == "--waiter-dies")
		{
			kernel::event value{};
			unsigned calls = 0;
			q.handle->receive(9, 100, 0, value, [&] { if (++calls == 2) _exit(0); return false; });
			std::abort();
		}
		check(q.handle->destroy(true) == status::not_owner);
		check(k.open_event(100, mode::create).code == status::exists);
		kernel::event result{};
		check(q.handle->receive(1, 500, 2'000'000, result, never) == status::ok);
		check(result.source == 0x1234 && result.data1 == 1 && result.data2 == 2 && result.data3 == 3);
		check(q.handle->send({0x5678, 4, 5, 6}) == status::ok);
		return 0;
	}
	auto console = memory::create(error);
	kernel k{console, error};
	check(k.open_event(100, mode::open).code == status::not_found);
	auto q = k.open_event(100, mode::create, protocol::fifo, 2);
	check(q.code == status::ok);
	const auto dead_waiter = launch(argv[0], "--waiter-dies", console.fd());
	join(dead_waiter);
	until([&] { check(q.handle->alive() == status::ok); return q.handle->waiter_count() == 0; });
	const auto child = launch(argv[0], "--receiver", console.fd());
	until([&] { return q.handle->waiter_count() == 1; });
	check(q.handle->destroy(false) == status::busy);
	check(q.handle->send({0x1234, 1, 2, 3}) == status::ok);
	join(child);
	kernel::event result{};
	check(q.handle->receive(1, 100, 50'000, result, never) == status::ok);
	check(result.source == 0x5678 && result.data3 == 6);
	check(q.handle->receive(1, 100, 5'000, result, never) == status::timed_out);
	check(q.handle->send({1, 0, 0, 0}) == status::ok);
	check(q.handle->send({2, 0, 0, 0}) == status::ok);
	check(q.handle->send({3, 0, 0, 0}) == status::busy);
	std::array<kernel::event, 2> batch{};
	std::size_t count;
	check(q.handle->try_receive(batch, count) == status::ok && count == 2);
	check(batch[0].source == 1 && batch[1].source == 2);
	check(q.handle->send({3, 0, 0, 0}) == status::ok);
	check(q.handle->drain() == status::ok);
	check(q.handle->try_receive(batch, count) == status::ok && !count);
	for (auto ordering : {protocol::fifo, protocol::priority})
	{
		auto ordered = k.open_event(101, mode::create, ordering, 3);
		check(ordered.code == status::ok);
		std::array<unsigned, 3> received{};
		std::vector<std::thread> threads;
		for (unsigned i = 0; i < 3; ++i)
		{
			threads.emplace_back([&, i] { kernel::event value{}; check(ordered.handle->receive(i+1, 1000-static_cast<int>(i)*100, 2'000'000, value, never) == status::ok); received[i] = value.source; });
			until([&] { return ordered.handle->waiter_count() == i+1; });
		}
		for (unsigned i = 0; i < 3; ++i) check(ordered.handle->send({i+1, 0, 0, 0}) == status::ok);
		for (auto& thread : threads) thread.join();
		check(received[0] == (ordering == protocol::fifo ? 1u : 3u));
		check(received[1] == 2 && received[2] == (ordering == protocol::fifo ? 3u : 1u));
	}
	std::atomic<bool> stop = false;
	std::thread cancelled([&] { kernel::event value{}; check(q.handle->receive(2, 100, 0, value, [&] { return stop.load(); }) == status::cancelled); });
	until([&] { return q.handle->waiter_count() == 1; });
	stop = true;
	cancelled.join();
	std::thread destroyed([&] { kernel::event value{}; check(q.handle->receive(3, 100, 0, value, never) == status::cancelled); });
	until([&] { return q.handle->waiter_count() == 1; });
	check(q.handle->destroy(true) == status::ok);
	destroyed.join();
	check(q.handle->send({}) == status::not_found);
	check(k.open_event(100, mode::open).code == status::not_found);
	auto replacement = k.open_event(100, mode::create);
	check(replacement.code == status::ok && replacement.handle->alive() == status::ok);
	check(q.handle->alive() == status::not_found); // Old ports cannot reach a recreated key.
	auto phase = console.map(memory::sysutil_key);
	auto* flag = reinterpret_cast<unsigned*>(phase.bytes().data());
	const auto creator = launch(argv[0], "--creator-dies", console.fd());
	until([&] { return __atomic_load_n(flag, __ATOMIC_ACQUIRE) == 1; });
	auto orphan = k.open_event(200, mode::open);
	check(orphan.code == status::ok);
	__atomic_store_n(flag, 2u, __ATOMIC_RELEASE);
	join(creator);
	until([&] { return orphan.handle->alive() == status::not_found; });
	auto isolated = memory::create(error);
	kernel other{isolated, error};
	check(other.open_event(100, mode::open).code == status::not_found);
	{
		std::vector<kernel::event_handle> handles;
		for (unsigned i = 0; i < kernel::max_event_queues; ++i)
		{
			auto entry = other.open_event(1000+i, mode::create);
			check(entry.code == status::ok);
			handles.push_back(std::move(*entry.handle));
		}
		check(other.open_event(9000, mode::create).code == status::no_space);
		handles.pop_back();
		check(other.open_event(9000, mode::create).code == status::ok);
	}
	check(other.open_event(1, mode::create, protocol::fifo, 0).code == status::invalid);
	check(other.open_event(1, mode::create, protocol::fifo, 128).code == status::invalid);
	check(other.open_event(1, mode::create, protocol::priority_inherit).code == status::invalid);
	std::cout << "PASS: exec-separated events, bounded FIFO, priority receivers, drain, timeout, cancellation, forced destroy, stale ports, creator/receiver death, capacity/reuse and console isolation\n";
}
