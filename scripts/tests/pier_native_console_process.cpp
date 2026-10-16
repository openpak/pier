#include "../../rpcs3/Emu/Cell/lv2/native_console_child.h"
#include <iostream>
#include <system_error>
extern char** environ;
using memory = pier::native_console_memory;
using kernel = pier::native_console_sync;
using status = kernel::status;
using state = kernel::guest_state;
using child = pier::native_console_child;
[[noreturn]] void error(const char* message, int value) { throw std::system_error(value, std::generic_category(), message); }
void check(bool value) { if (!value) { std::cerr << "process regression failed\n"; std::abort(); } }
template <typename Predicate> void until(Predicate predicate)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate()) { check(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
int main(int argc, char** argv)
{
	if (argc == 2)
	{
		auto console = memory::attach(198, error);
		close(198);
		kernel k{console, error};
		check(k.register_root_guest().code == status::exists);
		auto token = pier::native_guest_token_parse(std::getenv("PIER_NATIVE_GUEST_TOKEN"));
		check(token.has_value());
		auto invalid = *token;
		invalid.words[0] ^= 1;
		check(k.claim_guest_child(invalid).code == status::not_found);
		check(pier::native_guest_parent_death(std::stoi(std::getenv("PIER_NATIVE_PARENT_HOST"))) == 0);
		auto guest = k.claim_guest_child(*token);
		check(guest.code == status::ok && guest.info.pid > 1 && guest.info.parent > 0);
		check(k.current_guest().info.pid == guest.info.pid);
		auto thread = k.allocate_ppu_thread(0);
		check(thread.code == status::ok && thread.id != 0x01000000);
		check(k.resolve_ppu_thread(thread.id).actor == thread.actor);
		if (std::string_view(argv[1]) == "--target")
		{
			auto mutex = k.open_mutex(300, kernel::open_mode::open, kernel::protocol::fifo, false);
			check(mutex.code == status::ok);
			auto condition = k.open_condition(301, kernel::open_mode::open, *mutex.handle);
			check(condition.code == status::ok);
			check(mutex.handle->try_lock(thread.id) == status::ok);
			auto shared = console.map(memory::sysutil_key);
			__atomic_store_n(reinterpret_cast<std::uint32_t*>(shared.bytes().data()), thread.id, __ATOMIC_RELEASE);
			check(condition.handle->wait(thread.id, 100, 2'000'000, [] { return false; }) == status::ok);
			check(mutex.handle->unlock(thread.id) == status::ok);
		}
		check(k.claim_guest_child(*token).code == status::busy);
		check(k.reap_guest_child(1) == status::not_owner);
		if (std::string_view(argv[1]) == "--owner")
		{
			check(k.reserve_guest_child().code == status::ok); // Unclaimed reservation also dies with its parent.
			auto nested = child::spawn(k, console, argv[0], {"--wait"}, environ);
			check(!nested.error);
			until([&] { return k.inspect_guest(nested.child->guest_pid()).info.state == state::running; });
			auto shared = console.map(memory::sysutil_key);
			__atomic_store_n(reinterpret_cast<std::uint32_t*>(shared.bytes().data()), nested.child->guest_pid(), __ATOMIC_RELEASE);
			for (;;) pause(); // Parent test kills us outside all metadata transactions.
		}
		if (std::string_view(argv[1]) == "--crash") _exit(9);
		if (std::string_view(argv[1]) == "--wait")
		{
			for (;;) pause();
		}
		check(k.release_ppu_thread(thread.id) == status::ok);
		check(k.finish_guest(-42) == status::ok);
		check(k.finish_guest(0) == status::not_found);
		return 0;
	}
	check(!pier::native_guest_token_parse(""));
	check(!pier::native_guest_token_parse(std::string(32, '0')));
	check(!pier::native_guest_token_parse(std::string(32, 'g')));
	auto console = memory::create(error);
	kernel supervisor{console, error};
	check(supervisor.current_guest().code == status::not_found);
	check(supervisor.reserve_guest_child().code == status::not_owner);
	auto root = supervisor.register_root_guest();
	check(root.code == status::ok && root.info.pid == 1 && root.info.parent == 0);
	check(supervisor.register_root_guest().info.pid == 1);
	kernel alias{console, error};
	check(alias.current_guest().info.pid == 1); // One host peer, multiple attachments.
	auto reserved = supervisor.reserve_guest_child();
	check(reserved.code == status::ok && reserved.info.pid == 2);
	check(supervisor.claim_guest_child(reserved.token).code == status::busy);
	check(supervisor.reap_guest_child(2) == status::busy);
	check(supervisor.child_host_exited(2) == status::ok);
	check(supervisor.inspect_guest(2).info.state == state::host_failed);
	check(supervisor.reap_guest_child(2) == status::ok);
	check(supervisor.inspect_guest(2).code == status::not_found);
	check(pier::native_guest_token_parse(pier::native_guest_token_string(reserved.token)).has_value());
	auto root_thread = supervisor.allocate_ppu_thread(0);
	check(root_thread.code == status::ok && root_thread.id == 0x01000000);
	auto mutex = supervisor.open_mutex(300, kernel::open_mode::create, kernel::protocol::fifo, false);
	check(mutex.code == status::ok);
	auto condition = supervisor.open_condition(301, kernel::open_mode::create, *mutex.handle);
	check(condition.code == status::ok);
	std::uint32_t previous = 2;
	for (const auto task : {"--exit", "--crash", "--wait", "--target"})
	{
		auto spawned = child::spawn(supervisor, console, argv[0], {task}, environ);
		check(!spawned.error && spawned.child);
		const auto pid = spawned.child->guest_pid();
		check(pid > previous);
		previous = pid;
		until([&] { return supervisor.inspect_guest(pid).info.state != state::reserved; });
		if (std::string_view(task) == "--target")
		{
			until([&] { return condition.handle->waiter_count() == 1; });
			auto shared = console.map(memory::sysutil_key);
			const auto id = __atomic_load_n(reinterpret_cast<std::uint32_t*>(shared.bytes().data()), __ATOMIC_ACQUIRE);
			auto target = supervisor.resolve_ppu_thread(id);
			check(target.code == status::ok && id != root_thread.id);
			check(supervisor.release_ppu_thread(id) == status::not_owner);
			check(condition.handle->signal(false, root_thread.actor) == status::not_found);
			check(condition.handle->signal(false, target.actor) == status::ok);
		}
		if (std::string_view(task) == "--wait") spawned.child->stop();
		until([&] { return spawned.child->poll() == 0; });
		check(spawned.child->host_wait_status().has_value());
		auto result = supervisor.inspect_guest(pid);
		check(result.code == status::ok && result.info.parent == 1);
		if (std::string_view(task) == "--exit" || std::string_view(task) == "--target")
			check(result.info.state == state::exited && result.info.exit_status == -42);
		else check(result.info.state == state::host_failed);
		check(supervisor.current_guest().info.state == state::running);
		spawned.child.reset();
		check(supervisor.inspect_guest(pid).code == status::not_found);
	}
	std::uint32_t held = 0;
	{
		// VSH's wait path: supervisor-owned spawn, blocking wait, reap on return.
		std::vector<std::string> environment;
		for (auto entry = environ; *entry; ++entry) environment.emplace_back(*entry);
		pier::native_child_supervisor children{supervisor, console};
		auto never = [] { return false; };
		check(children.wait(1, never).error == ESRCH);
		auto exit = children.spawn(argv[0], {"--exit"}, environment);
		auto crash = children.spawn(argv[0], {"--crash"}, environment);
		auto hold = children.spawn(argv[0], {"--wait"}, environment);
		check(!exit.error && !crash.error && !hold.error && children.running() >= 1);
		auto exited = children.wait(exit.pid, never);
		check(!exited.error && exited.info.state == state::exited && exited.info.exit_status == -42);
		check(supervisor.inspect_guest(exit.pid).code == status::not_found && children.wait(exit.pid, never).error == ESRCH);
		auto crashed = children.wait(crash.pid, never);
		check(!crashed.error && crashed.info.state == state::host_failed && WIFEXITED(crashed.host_status) && WEXITSTATUS(crashed.host_status) == 9);
		check(children.wait(hold.pid, [] { return true; }).error == ECANCELED);
		until([&] { return supervisor.inspect_guest(hold.pid).info.state == state::running; });
		check(children.running() == 1);
		check(children.spawn("/nonexistent/pier-child-test", {}, environment).error == ENOENT);
		held = hold.pid;
	}
	// Supervisor destruction terminated and reaped the child it still owned.
	check(supervisor.inspect_guest(held).code == status::not_found);
	{
		auto shared = console.map(memory::sysutil_key);
		auto* nested_pid = reinterpret_cast<std::uint32_t*>(shared.bytes().data());
		__atomic_store_n(nested_pid, 0u, __ATOMIC_RELEASE);
		auto owner = child::spawn(supervisor, console, argv[0], {"--owner"}, environ);
		check(!owner.error);
		until([&] { return __atomic_load_n(nested_pid, __ATOMIC_ACQUIRE) != 0; });
		const auto nested = __atomic_load_n(nested_pid, __ATOMIC_ACQUIRE);
		check(supervisor.inspect_guest(nested).info.state == state::running);
		check(kill(owner.child->host_pid(), SIGKILL) == 0);
		until([&] { return owner.child->poll() == 0; });
		until([&] { return supervisor.inspect_guest(nested).info.state == state::host_failed; });
	}
	auto failed = child::spawn(supervisor, console, "/nonexistent/pier-child-test", {}, environ);
	check(failed.error == ENOENT && !failed.child);
	// Reservations consume table slots until the parent explicitly reaps them.
	std::vector<std::uint32_t> pending;
	for (;;)
	{
		auto entry = supervisor.reserve_guest_child();
		if (entry.code == status::no_space) break;
		check(entry.code == status::ok);
		pending.push_back(entry.info.pid);
	}
	check(pending.size() == 31); // Terminal orphans are collected after their parent is reaped.
	for (auto pid : pending) { check(supervisor.child_host_exited(pid) == status::ok); check(supervisor.reap_guest_child(pid) == status::ok); }
	auto another = supervisor.reserve_guest_child();
	check(another.code == status::ok && another.info.pid > pending.back());
	check(supervisor.child_host_exited(another.info.pid) == status::ok);
	check(supervisor.reap_guest_child(another.info.pid) == status::ok);
	auto isolated = memory::create(error);
	kernel other{isolated, error};
	check(other.claim_guest_child(reserved.token).code == status::not_found);
	check(other.register_root_guest().info.pid == 1);
	check(other.claim_guest_child(reserved.token).code == status::busy);
	check(supervisor.release_ppu_thread(root_thread.id) == status::ok);
	check(supervisor.resolve_ppu_thread(root_thread.id).code == status::not_found);
	auto replacement_thread = supervisor.allocate_ppu_thread(0);
	check(replacement_thread.code == status::ok && replacement_thread.id != root_thread.id);
	check(supervisor.allocate_ppu_thread(100).code == status::invalid);
	for (unsigned generation = 0; generation < 4096; ++generation)
	{
		auto entry = supervisor.allocate_ppu_thread(99);
		check(entry.code == status::ok && ((entry.id >> 12) & 4095) == generation);
		check(supervisor.release_ppu_thread(entry.id) == status::ok);
	}
	check(supervisor.allocate_ppu_thread(99).code == status::no_space);
	check(supervisor.allocate_ppu_thread(98).code == status::ok);
	std::cout << "PASS: real guest PID/parent registration, one-use child capability, separate exec exit/crash/stop, supervisor spawn/wait/cancel/teardown, parent survival, globally distinct PPU IDs/targeted condition signal, failed spawn cleanup, parent-death termination, capacity/reap and console isolation\n";
}
