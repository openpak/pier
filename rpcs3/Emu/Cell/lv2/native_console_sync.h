#pragma once

#ifdef __linux__
#include "native_console_memory.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <signal.h>
#include <linux/futex.h>

namespace pier
{
// Host kernel state contains indices and console-scoped actor IDs, never guest
// addresses, C++ pointers, or PPU wait queues. LV2 syscall routing is separate.
class native_console_sync
{
public:
	enum class status : std::uint32_t
	{
		ok, not_found, exists, invalid, busy, deadlock, not_owner, overflow,
		timed_out, cancelled, owner_dead, no_space, unsupported, broken, has_conditions,
	};
	enum class open_mode : std::uint32_t { create = 1, open = 2, open_or_create = 3 };
	enum class protocol : std::uint32_t { fifo = 1, priority = 2, priority_inherit = 3 };
	static constexpr std::size_t max_processes = 16;
	static constexpr std::size_t max_mutexes = 64;
	static constexpr std::size_t max_conditions = 64;
	static constexpr std::size_t max_waiters = 512;
	static constexpr std::size_t max_event_queues = 32;
	static constexpr std::size_t max_event_waiters = 256;
	struct event { std::uint64_t source, data1, data2, data3; };
	enum class guest_state : std::uint32_t { reserved = 1, running, exited, host_failed };
	struct guest_token { std::uint64_t words[2]; };
	struct guest_info { std::uint32_t pid, parent; guest_state state; std::int32_t exit_status; };
	struct guest_result { status code; guest_info info{}; guest_token token{}; };

private:
	struct process_record
	{
		std::uint64_t start_ticks;
		std::int32_t pid;
		std::uint32_t peer;
		std::uint32_t attachments;
	};
	struct mutex_record
	{
		std::uint64_t key;
		std::uint64_t owner;
		std::uint32_t generation;
		std::uint32_t depth;
		std::uint32_t protocol;
		std::uint32_t recursive;
		std::uint32_t owner_dead;
		std::uint32_t wake;
		std::uint32_t conditions;
		std::uint32_t refs[max_processes];
	};
	struct condition_record
	{
		std::uint64_t key;
		std::uint32_t generation;
		std::uint32_t mutex;
		std::uint32_t mutex_generation;
		std::uint32_t refs[max_processes];
	};
	struct waiter_record
	{
		std::uint64_t actor;
		std::uint64_t order;
		std::int32_t priority;
		std::uint32_t mutex;
		std::uint32_t generation;
		std::uint32_t state; // 0=free, 1=mutex queue, 2=granted, 3=condition queue
		std::uint32_t condition; // Original condition index + 1, or zero.
		std::uint32_t depth;
		std::uint32_t result;
	};
	struct event_queue_record
	{
		std::uint64_t key;
		std::uint32_t generation, protocol, capacity, head, count, destroyed, creator, wake;
		std::uint32_t refs[max_processes];
		event events[127];
	};
	struct event_waiter_record
	{
		std::uint64_t actor, order;
		std::int32_t priority;
		std::uint32_t queue, generation, state; // 1=waiting, 2=delivered
		event payload;
	};
	struct guest_record
	{
		std::uint32_t pid, parent, peer;
		guest_state state;
		std::int32_t exit_status;
		guest_token token;
	};
	static constexpr std::size_t max_guests = 32;
	struct thread_record { std::uint32_t id, guest, peer; };
	static constexpr std::size_t max_threads = 1600;
	struct arena
	{
		native_console_control control;
		std::uint32_t version;
		std::uint32_t abi_size;
		std::uint32_t next_peer;
		std::uint32_t next_guest;
		guest_record guests[max_guests];
		std::uint32_t next_thread_generation[100];
		thread_record threads[max_threads];
		std::uint64_t next_waiter;
		std::uint64_t last_reap_ns;
		process_record processes[max_processes];
		mutex_record mutexes[max_mutexes];
		condition_record conditions[max_conditions];
		waiter_record waiters[max_waiters];
		event_queue_record event_queues[max_event_queues];
		event_waiter_record event_waiters[max_event_waiters];
	};
	static_assert(sizeof(arena) <= native_console_memory::control_size);

	class guard
	{
		native_console_control& m_control;
		bool m_locked = false;
		bool m_valid = false;
	public:
		explicit guard(native_console_control& control) : m_control(control)
		{
			const int result = ::pthread_mutex_lock(&control.lock);
			m_locked = result == 0 || result == EOWNERDEAD;
			if (result == EOWNERDEAD)
			{
				// A host died while mutating metadata. Its partial transaction is
				// unknowable; poison this console instead of repairing it blindly.
				control.poisoned = 1;
				::pthread_mutex_consistent(&control.lock);
			}
			m_valid = result == 0 && !control.poisoned;
		}
		~guard() { if (m_locked) ::pthread_mutex_unlock(&m_control.lock); }
		explicit operator bool() const noexcept { return m_valid; }
	};

	static std::uint64_t now_ns()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static std::uint64_t start_ticks(pid_t pid, bool* exited = nullptr)
	{
		std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
		std::string line;
		if (!std::getline(input, line)) return 0;
		const auto end_name = line.rfind(')');
		if (end_name == std::string::npos) return 0;
		std::string_view fields{line.data() + end_name + 1, line.size() - end_name - 1};
		for (int field = 3; field <= 22; ++field)
		{
			const auto begin = fields.find_first_not_of(' ');
			if (begin == std::string_view::npos) return 0;
			fields.remove_prefix(begin);
			const auto end = fields.find(' ');
			const auto token = fields.substr(0, end);
			if (field == 3 && exited) *exited = token == "Z" || token == "X";
			if (field == 22)
			{
				std::uint64_t result = 0;
				const auto [last, error] = std::from_chars(token.data(), token.data() + token.size(), result);
				return error == std::errc{} && last == token.data() + token.size() ? result : 0;
			}
			if (end == std::string_view::npos) return 0;
			fields.remove_prefix(end + 1);
		}
		return 0;
	}
	static void wake(mutex_record& mutex)
	{
		__atomic_add_fetch(&mutex.wake, 1u, __ATOMIC_RELEASE);
		::syscall(SYS_futex, &mutex.wake, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
	}
	template <typename Record> static bool referenced(const Record& mutex)
	{
		return std::any_of(std::begin(mutex.refs), std::end(mutex.refs), [](auto count) { return count != 0; });
	}
	static bool queued(const arena& state, std::size_t index)
	{
		return std::any_of(std::begin(state.waiters), std::end(state.waiters), [&](const auto& waiter)
		{
			return waiter.state && waiter.mutex == index && waiter.generation == state.mutexes[index].generation;
		});
	}
	static void collect(arena& state, std::size_t index)
	{
		auto& mutex = state.mutexes[index];
		if (mutex.key && !referenced(mutex) && !mutex.conditions && !queued(state, index))
		{
			// Preserve generation and wake counter for stale-handle rejection.
			mutex.key = 0;
			mutex.owner = 0;
			mutex.depth = 0;
			mutex.owner_dead = 0;
		}
	}
	static void collect_condition(arena& state, std::size_t index)
	{
		auto& condition = state.conditions[index];
		if (!condition.key || referenced(condition)) return;
		for (const auto& waiter : state.waiters)
			if (waiter.state && waiter.condition == index + 1) return;
		auto& mutex = state.mutexes[condition.mutex];
		if (mutex.generation == condition.mutex_generation && mutex.conditions) --mutex.conditions;
		condition.key = 0;
		collect(state, condition.mutex);
	}
	template <typename Record> static void event_wake(Record& queue)
	{
		__atomic_add_fetch(&queue.wake, 1u, __ATOMIC_RELEASE);
		::syscall(SYS_futex, &queue.wake, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
	}
	static bool event_waiting(const arena& state, std::size_t index)
	{
		return std::any_of(std::begin(state.event_waiters), std::end(state.event_waiters), [&](const auto& waiter)
		{
			return waiter.state && waiter.queue == index && waiter.generation == state.event_queues[index].generation;
		});
	}
	static void collect_event(arena& state, std::size_t index)
	{
		auto& queue = state.event_queues[index];
		if (queue.key && !referenced(queue) && !event_waiting(state, index))
		{
			queue.key = 0;
			queue.count = queue.head = queue.destroyed = queue.creator = 0;
		}
	}
	static void remove_peer(arena& state, std::size_t slot)
	{
		const auto peer = state.processes[slot].peer;
		for (auto& waiter : state.waiters)
			if (waiter.state && waiter.actor >> 32 == peer) waiter.state = 0;
		for (std::size_t index = 0; index < max_conditions; ++index)
		{
			state.conditions[index].refs[slot] = 0;
			collect_condition(state, index);
		}
		for (std::size_t index = 0; index < max_mutexes; ++index)
		{
			auto& mutex = state.mutexes[index];
			mutex.refs[slot] = 0;
			if (mutex.owner && mutex.owner >> 32 == peer)
				mutex.owner_dead = 1;
			wake(mutex);
			collect(state, index);
		}
		for (auto& waiter : state.event_waiters)
			if (waiter.state && waiter.actor >> 32 == peer) waiter.state = 0;
		for (std::size_t index = 0; index < max_event_queues; ++index)
		{
			auto& queue = state.event_queues[index];
			queue.refs[slot] = 0;
			if (queue.creator == peer) queue.destroyed = 1;
			event_wake(queue);
			collect_event(state, index);
		}
		for (auto& guest : state.guests)
			if (guest.pid && guest.peer == peer && guest.state == guest_state::running)
				guest.state = guest_state::host_failed;
		// Unclaimed reservations cannot survive their guest parent.
		for (auto& guest : state.guests)
		{
			if (!guest.pid || guest.state != guest_state::reserved) continue;
			for (const auto& parent : state.guests)
				if (parent.pid == guest.parent && parent.peer == peer)
				{ guest.state = guest_state::host_failed; guest.token = {}; break; }
		}
		for (auto& thread : state.threads)
			if (thread.peer == peer) thread = {};
		state.processes[slot] = {};
	}
	static void collect_guest_orphans(arena& state)
	{
		bool removed;
		do
		{
			removed = false;
			for (auto& guest : state.guests)
			{
				if (!guest.pid || !guest.parent || guest.state == guest_state::running || guest.state == guest_state::reserved) continue;
				bool parent_exists = false;
				for (const auto& parent : state.guests) if (parent.pid == guest.parent) parent_exists = true;
				if (!parent_exists) { guest = {}; removed = true; }
			}
		} while (removed);
	}
	static void reap(arena& state)
	{
		const auto now = now_ns();
		if (now - state.last_reap_ns < 100'000'000) return;
		state.last_reap_ns = now;
		for (std::size_t index = 0; index < max_processes; ++index)
		{
			const auto& process = state.processes[index];
			if (!process.peer) continue;
			bool exited = false;
			const auto started = start_ticks(process.pid, &exited);
			const bool gone = ::kill(process.pid, 0) < 0 && errno == ESRCH;
			if (gone || exited || (started && started != process.start_ticks)) remove_peer(state, index);
		}
		collect_guest_orphans(state);
	}
	static void handoff(arena& state, std::size_t index)
	{
		auto& mutex = state.mutexes[index];
		waiter_record* next = nullptr;
		for (auto& waiter : state.waiters)
		{
			if (waiter.state != 1 || waiter.mutex != index || waiter.generation != mutex.generation) continue;
			if (!next || (mutex.protocol == 2 && waiter.priority < next->priority) ||
				((mutex.protocol == 1 || waiter.priority == next->priority) && waiter.order < next->order)) next = &waiter;
		}
		mutex.owner = next ? next->actor : 0;
		mutex.depth = next ? next->depth : 0;
		if (next) next->state = 2;
		wake(mutex);
	}

	struct session
	{
		native_console_memory::mapping mapping;
		arena& state;
		std::size_t slot = max_processes;
		std::uint32_t peer = 0;
		const pid_t host_pid = ::getpid();

		session(const native_console_memory& console, native_console_error_handler error)
			: mapping(console.map_control()), state(*reinterpret_cast<arena*>(mapping.bytes().data()))
		{
			auto fail = [&](const char* message, int code)
			{
				error(message, code);
				std::abort();
			};
			guard lock{state.control};
			if (!lock) fail("broken native console control", EOWNERDEAD);
			if (!state.version)
			{
				state.abi_size = sizeof(arena);
				state.version = 4;
			}
			if (state.version != 4 || state.abi_size != sizeof(arena)) fail("incompatible native console synchronization", EINVAL);
			reap(state);
			const auto pid = ::getpid();
			const auto started = start_ticks(pid);
			if (!started) fail("native console process identity unavailable", ENOTSUP);
			for (std::size_t index = 0; index < max_processes; ++index)
			{
				auto& process = state.processes[index];
				if (process.peer && process.pid == pid && process.start_ticks == started)
				{
					if (process.attachments == UINT_MAX) fail("native console attachment overflow", EOVERFLOW);
					++process.attachments;
					slot = index;
					peer = process.peer;
					return;
				}
			}
			for (std::size_t index = 0; index < max_processes; ++index)
			{
				if (!state.processes[index].peer)
				{
					if (state.next_peer == UINT_MAX) fail("native console process identity exhausted", EOVERFLOW);
					peer = ++state.next_peer;
					slot = index;
					state.processes[index] = {started, pid, peer, 1};
					return;
				}
			}
			fail("native console process table full", ENOSPC);
		}
		~session()
		{
			if (host_pid != ::getpid()) return;
			guard lock{state.control};
			if (lock && slot < max_processes && state.processes[slot].peer == peer && !--state.processes[slot].attachments)
				remove_peer(state, slot);
		}
	};
	std::shared_ptr<session> m_session;

public:
	class condition_handle;
	class mutex_handle
	{
		friend class native_console_sync;
		friend class condition_handle;
		std::shared_ptr<session> m_session;
		std::size_t m_index;
		std::uint32_t m_generation;
		mutex_handle(std::shared_ptr<session> owner, std::size_t index, std::uint32_t generation)
			: m_session(std::move(owner)), m_index(index), m_generation(generation) {}

		mutex_record* record() const
		{
			auto& result = m_session->state.mutexes[m_index];
			return result.key && result.generation == m_generation ? &result : nullptr;
		}
		std::uint64_t actor(std::uint32_t thread) const { return std::uint64_t{m_session->peer} << 32 | thread; }
		status acquire(mutex_record& mutex, std::uint64_t who) const
		{
			if (mutex.owner_dead) return status::owner_dead;
			if (!mutex.owner)
			{
				mutex.owner = who;
				mutex.depth = 1;
				return status::ok;
			}
			if (mutex.owner != who) return status::busy;
			if (!mutex.recursive) return status::deadlock;
			if (mutex.depth == UINT_MAX) return status::overflow;
			++mutex.depth;
			return status::ok;
		}

	public:
		mutex_handle(const mutex_handle&) = delete;
		mutex_handle& operator=(const mutex_handle&) = delete;
		mutex_handle(mutex_handle&&) noexcept = default;
		mutex_handle& operator=(mutex_handle&&) = delete;
		~mutex_handle()
		{
			if (!m_session || m_session->host_pid != ::getpid()) return;
			guard lock{m_session->state.control};
			if (lock)
			{
				if (auto* mutex = record(); mutex && mutex->refs[m_session->slot]) --mutex->refs[m_session->slot];
				collect(m_session->state, m_index);
			}
		}

		status try_lock(std::uint32_t thread)
		{
			if (!thread || m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			reap(m_session->state);
			auto* mutex = record();
			return mutex ? acquire(*mutex, actor(thread)) : status::not_found;
		}

		template <typename Cancel>
		status lock(std::uint32_t thread, std::int32_t priority, std::uint64_t timeout_us, Cancel&& cancel)
		{
			if (!thread || m_session->host_pid != ::getpid()) return status::invalid;
			auto& state = m_session->state;
			const auto who = actor(thread);
			const auto started = now_ns();
			std::size_t queued_index = max_waiters;
			for (;;)
			{
				const bool cancelled = cancel();
				std::uint32_t sequence;
				{
					guard lock{state.control};
					if (!lock) return status::broken;
					reap(state);
					auto* mutex = record();
					if (!mutex) return status::not_found;
					auto leave = [&](status result)
					{
						if (queued_index < max_waiters) state.waiters[queued_index].state = 0;
						return result;
					};
					if (queued_index < max_waiters && state.waiters[queued_index].state == 2 && mutex->owner == who)
						return leave(status::ok);
					if (mutex->owner_dead) return leave(status::owner_dead);
					if (cancelled) return leave(status::cancelled);
					if (queued_index == max_waiters)
					{
						const auto result = acquire(*mutex, who);
						if (result != status::busy) return result;
						for (std::size_t index = 0; index < max_waiters; ++index)
						{
							if (!state.waiters[index].state) { queued_index = index; break; }
						}
						if (queued_index == max_waiters || state.next_waiter == std::numeric_limits<std::uint64_t>::max()) return status::no_space;
						state.waiters[queued_index] = {who, ++state.next_waiter, priority, static_cast<std::uint32_t>(m_index), m_generation, 1, 0, 1, 0};
					}
					if (timeout_us && (now_ns() - started) / 1000 >= timeout_us) return leave(status::timed_out);
					sequence = __atomic_load_n(&mutex->wake, __ATOMIC_ACQUIRE);
				}
				// Bounded waits allow emulator stop/cancellation and peer death to
				// be observed even when no surviving peer sends a wakeup.
				const timespec delay{0, 5'000'000};
				if (::syscall(SYS_futex, &state.mutexes[m_index].wake, FUTEX_WAIT, sequence, &delay, nullptr, 0) < 0 &&
					errno != EINTR && errno != EAGAIN && errno != ETIMEDOUT)
				{
					guard lock{state.control};
					if (lock) state.control.poisoned = 1;
					return status::broken;
				}
			}
		}

		status unlock(std::uint32_t thread)
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			reap(m_session->state);
			auto* mutex = record();
			if (!mutex) return status::not_found;
			if (mutex->owner_dead) return status::owner_dead;
			if (!thread || mutex->owner != actor(thread)) return status::not_owner;
			if (--mutex->depth == 0) handoff(m_session->state, m_index);
			return status::ok;
		}

		status idle() const
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			const auto* mutex = record();
			if (!mutex) return status::not_found;
			if (mutex->owner || queued(m_session->state, m_index)) return status::busy;
			return mutex->conditions ? status::has_conditions : status::ok;
		}

		std::size_t waiter_count() const
		{
			if (m_session->host_pid != ::getpid()) return 0;
			guard lock{m_session->state.control};
			if (!lock) return 0;
			return std::count_if(std::begin(m_session->state.waiters), std::end(m_session->state.waiters), [&](const auto& waiter)
			{
				return waiter.state == 1 && waiter.mutex == m_index && waiter.generation == m_generation;
			});
		}
	};

	class condition_handle
	{
		friend class native_console_sync;
		std::shared_ptr<session> m_session;
		std::size_t m_index;
		std::uint32_t m_generation;
		condition_handle(std::shared_ptr<session> owner, std::size_t index, std::uint32_t generation)
			: m_session(std::move(owner)), m_index(index), m_generation(generation) {}
		condition_record* record() const
		{
			auto& result = m_session->state.conditions[m_index];
			return result.key && result.generation == m_generation ? &result : nullptr;
		}

	public:
		condition_handle(const condition_handle&) = delete;
		condition_handle& operator=(const condition_handle&) = delete;
		condition_handle(condition_handle&&) noexcept = default;
		condition_handle& operator=(condition_handle&&) = delete;
		~condition_handle()
		{
			if (!m_session || m_session->host_pid != ::getpid()) return;
			guard lock{m_session->state.control};
			if (!lock) return;
			if (auto* condition = record(); condition && condition->refs[m_session->slot]) --condition->refs[m_session->slot];
			collect_condition(m_session->state, m_index);
		}

		template <typename Cancel>
		status wait(std::uint32_t thread, std::int32_t priority, std::uint64_t timeout_us, Cancel&& cancel)
		{
			if (!thread || m_session->host_pid != ::getpid()) return status::invalid;
			if (cancel()) return status::cancelled;
			auto& state = m_session->state;
			const auto who = std::uint64_t{m_session->peer} << 32 | thread;
			const auto started = now_ns();
			std::size_t queued_index = max_waiters;
			std::size_t mutex_index;
			{
				guard lock{state.control};
				if (!lock) return status::broken;
				reap(state);
				auto* condition = record();
				if (!condition) return status::not_found;
				mutex_index = condition->mutex;
				auto& mutex = state.mutexes[mutex_index];
				if (mutex.owner_dead) return status::owner_dead;
				if (mutex.owner != who) return status::not_owner;
				for (std::size_t index = 0; index < max_waiters; ++index)
					if (!state.waiters[index].state) { queued_index = index; break; }
				if (queued_index == max_waiters || state.next_waiter == std::numeric_limits<std::uint64_t>::max()) return status::no_space;
				state.waiters[queued_index] = {who, ++state.next_waiter, priority, static_cast<std::uint32_t>(mutex_index),
					mutex.generation, 3, static_cast<std::uint32_t>(m_index + 1), mutex.depth, static_cast<std::uint32_t>(status::ok)};
				// Registration and release are one metadata transaction, preventing
				// the producer's signal from falling between them.
				mutex.owner = 0;
				mutex.depth = 0;
				handoff(state, mutex_index);
			}
			for (;;)
			{
				const bool cancelled = cancel();
				std::uint32_t sequence;
				{
					guard lock{state.control};
					if (!lock) return status::broken;
					reap(state);
					auto& mutex = state.mutexes[mutex_index];
					auto& waiter = state.waiters[queued_index];
					auto leave = [&](status result) { waiter.state = 0; return result; };
					if (waiter.state == 2 && mutex.owner == who) return leave(static_cast<status>(waiter.result));
					if (mutex.owner_dead) return leave(status::owner_dead);
					// Cancellation belongs to emulator teardown. It leaves the mutex
					// released; a normal timeout always reacquires it first.
					if (cancelled) return leave(status::cancelled);
					if (waiter.state == 3 && timeout_us && (now_ns() - started) / 1000 >= timeout_us)
					{
						if (state.next_waiter == std::numeric_limits<std::uint64_t>::max())
						{
							state.control.poisoned = 1;
							return status::broken;
						}
						waiter.result = static_cast<std::uint32_t>(status::timed_out);
						waiter.order = ++state.next_waiter;
						waiter.state = 1;
						if (!mutex.owner) handoff(state, mutex_index);
						continue;
					}
					sequence = __atomic_load_n(&mutex.wake, __ATOMIC_ACQUIRE);
				}
				const timespec delay{0, 5'000'000};
				if (::syscall(SYS_futex, &state.mutexes[mutex_index].wake, FUTEX_WAIT, sequence, &delay, nullptr, 0) < 0 &&
					errno != EINTR && errno != EAGAIN && errno != ETIMEDOUT)
				{
					guard lock{state.control};
					if (lock) state.control.poisoned = 1;
					return status::broken;
				}
			}
		}

		// target_actor is a console-scoped actor ID, not a host pointer or a
		// process-local PPU number. Zero selects an ordinary signal/broadcast.
		status signal(bool broadcast = false, std::uint64_t target_actor = 0)
		{
			if (m_session->host_pid != ::getpid() || (broadcast && target_actor)) return status::invalid;
			auto& state = m_session->state;
			guard lock{state.control};
			if (!lock) return status::broken;
			reap(state);
			auto* condition = record();
			if (!condition) return status::not_found;
			auto& mutex = state.mutexes[condition->mutex];
			if (mutex.owner_dead) return status::owner_dead;
			bool signaled = false;
			for (;;)
			{
				waiter_record* next = nullptr;
				for (auto& waiter : state.waiters)
				{
					if (waiter.state != 3 || waiter.condition != m_index + 1 || (target_actor && waiter.actor != target_actor)) continue;
					if (!next || (mutex.protocol == 2 && waiter.priority < next->priority) ||
						((mutex.protocol == 1 || waiter.priority == next->priority) && waiter.order < next->order)) next = &waiter;
				}
				if (!next) break;
				if (state.next_waiter == std::numeric_limits<std::uint64_t>::max())
				{
					state.control.poisoned = 1;
					return status::broken;
				}
				next->order = ++state.next_waiter;
				next->state = 1;
				signaled = true;
				if (!broadcast) break;
			}
			if (!mutex.owner) handoff(state, condition->mutex);
			else if (signaled) wake(mutex);
			return target_actor && !signaled ? status::not_found : status::ok;
		}

		std::size_t waiter_count() const
		{
			if (m_session->host_pid != ::getpid()) return 0;
			guard lock{m_session->state.control};
			if (!lock) return 0;
			return std::count_if(std::begin(m_session->state.waiters), std::end(m_session->state.waiters), [&](const auto& waiter)
			{
				return waiter.state == 3 && waiter.condition == m_index + 1;
			});
		}

		status idle() const
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			if (!record()) return status::not_found;
			for (const auto& waiter : m_session->state.waiters)
				if (waiter.state == 3 && waiter.condition == m_index + 1) return status::busy;
			return status::ok;
		}
	};

	class event_handle
	{
		friend class native_console_sync;
		std::shared_ptr<session> m_session;
		std::size_t m_index;
		std::uint32_t m_generation;
		event_handle(std::shared_ptr<session> owner, std::size_t index, std::uint32_t generation)
			: m_session(std::move(owner)), m_index(index), m_generation(generation) {}
		event_queue_record* record() const
		{
			auto& queue = m_session->state.event_queues[m_index];
			return queue.key && queue.generation == m_generation ? &queue : nullptr;
		}
	public:
		event_handle(const event_handle&) = delete;
		event_handle& operator=(const event_handle&) = delete;
		event_handle(event_handle&&) noexcept = default;
		event_handle& operator=(event_handle&&) = delete;
		~event_handle()
		{
			if (!m_session || m_session->host_pid != ::getpid()) return;
			guard lock{m_session->state.control};
			if (!lock) return;
			if (auto* queue = record(); queue && queue->refs[m_session->slot]) --queue->refs[m_session->slot];
			collect_event(m_session->state, m_index);
		}
		status alive() const
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			reap(m_session->state);
			const auto* queue = record();
			return !queue || queue->destroyed ? status::not_found : status::ok;
		}
		status destroy(bool force)
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			auto& state = m_session->state;
			guard lock{state.control};
			if (!lock) return status::broken;
			reap(state);
			auto* queue = record();
			if (!queue || queue->destroyed) return status::not_found;
			if (queue->creator != m_session->peer) return status::not_owner;
			if (!force && event_waiting(state, m_index)) return status::busy;
			queue->destroyed = 1;
			queue->count = 0;
			event_wake(*queue);
			return status::ok;
		}
		status send(event payload)
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			auto& state = m_session->state;
			guard lock{state.control};
			if (!lock) return status::broken;
			reap(state);
			auto* queue = record();
			if (!queue || queue->destroyed) return status::not_found;
			event_waiter_record* next = nullptr;
			for (auto& waiter : state.event_waiters)
			{
				if (waiter.state != 1 || waiter.queue != m_index || waiter.generation != m_generation) continue;
				if (!next || (queue->protocol == 2 && waiter.priority < next->priority) ||
					((queue->protocol == 1 || waiter.priority == next->priority) && waiter.order < next->order)) next = &waiter;
			}
			if (next)
			{
				next->payload = payload;
				next->state = 2;
				event_wake(*queue);
				return status::ok;
			}
			if (queue->count == queue->capacity) return status::busy;
			queue->events[(queue->head + queue->count++) % queue->capacity] = payload;
			return status::ok;
		}
		status drain()
		{
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			auto* queue = record();
			if (!queue || queue->destroyed) return status::not_found;
			queue->head = queue->count = 0;
			return status::ok;
		}
		status try_receive(std::span<event> output, std::size_t& count)
		{
			count = 0;
			if (m_session->host_pid != ::getpid()) return status::invalid;
			guard lock{m_session->state.control};
			if (!lock) return status::broken;
			reap(m_session->state);
			auto* queue = record();
			if (!queue || queue->destroyed) return status::not_found;
			while (count < output.size() && queue->count)
			{
				output[count++] = queue->events[queue->head];
				queue->head = (queue->head + 1) % queue->capacity;
				--queue->count;
			}
			return status::ok;
		}
		template <typename Cancel>
		status receive(std::uint32_t thread, std::int32_t priority, std::uint64_t timeout_us, event& output, Cancel&& cancel)
		{
			if (!thread || m_session->host_pid != ::getpid()) return status::invalid;
			if (cancel()) return status::cancelled;
			auto& state = m_session->state;
			const auto started = now_ns();
			std::size_t index = max_event_waiters;
			{
				guard lock{state.control};
				if (!lock) return status::broken;
				reap(state);
				auto* queue = record();
				if (!queue || queue->destroyed) return status::not_found;
				if (queue->count)
				{
					output = queue->events[queue->head];
					queue->head = (queue->head + 1) % queue->capacity;
					--queue->count;
					return status::ok;
				}
				for (std::size_t slot = 0; slot < max_event_waiters; ++slot)
					if (!state.event_waiters[slot].state) { index = slot; break; }
				if (index == max_event_waiters || state.next_waiter == UINT64_MAX) return status::no_space;
				state.event_waiters[index] = {std::uint64_t{m_session->peer} << 32 | thread, ++state.next_waiter,
					priority, static_cast<std::uint32_t>(m_index), m_generation, 1, {}};
			}
			for (;;)
			{
				const bool cancelled = cancel();
				std::uint32_t sequence;
				{
					guard lock{state.control};
					if (!lock) return status::broken;
					reap(state);
					auto& waiter = state.event_waiters[index];
					auto leave = [&](status value) { waiter.state = 0; return value; };
					// A completed delivery wins a concurrent destroy, timeout or stop.
					if (waiter.state == 2) { output = waiter.payload; return leave(status::ok); }
					auto* queue = record();
					if (!queue || queue->destroyed) return leave(status::cancelled);
					if (cancelled) return leave(status::cancelled);
					if (timeout_us && (now_ns() - started) / 1000 >= timeout_us) return leave(status::timed_out);
					sequence = __atomic_load_n(&queue->wake, __ATOMIC_ACQUIRE);
				}
				const timespec delay{0, 5'000'000};
				if (::syscall(SYS_futex, &state.event_queues[m_index].wake, FUTEX_WAIT, sequence, &delay, nullptr, 0) < 0 &&
					errno != EINTR && errno != EAGAIN && errno != ETIMEDOUT)
				{
					guard lock{state.control};
					if (lock) state.control.poisoned = 1;
					return status::broken;
				}
			}
		}
		std::size_t waiter_count() const
		{
			if (m_session->host_pid != ::getpid()) return 0;
			guard lock{m_session->state.control};
			if (!lock) return 0;
			return std::count_if(std::begin(m_session->state.event_waiters), std::end(m_session->state.event_waiters), [&](const auto& waiter)
			{ return waiter.state == 1 && waiter.queue == m_index && waiter.generation == m_generation; });
		}
	};
	struct event_result { status code; std::optional<event_handle> handle; };

	event_result open_event(std::uint64_t key, open_mode mode, protocol ordering = protocol::fifo, std::uint32_t capacity = 127)
	{
		if (m_session->host_pid != ::getpid() || !key || mode < open_mode::create || mode > open_mode::open_or_create) return {status::invalid, {}};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken, {}};
		reap(state);
		std::size_t index = max_event_queues;
		for (std::size_t slot = 0; slot < max_event_queues; ++slot)
			if (state.event_queues[slot].key == key && !state.event_queues[slot].destroyed) { index = slot; break; }
		if (index != max_event_queues)
		{
			if (mode == open_mode::create) return {status::exists, {}};
		}
		else
		{
			if (mode == open_mode::open) return {status::not_found, {}};
			if (!capacity || capacity > 127 || (ordering != protocol::fifo && ordering != protocol::priority)) return {status::invalid, {}};
			for (std::size_t slot = 0; slot < max_event_queues; ++slot)
				if (!state.event_queues[slot].key && state.event_queues[slot].generation != UINT_MAX) { index = slot; break; }
			if (index == max_event_queues) return {status::no_space, {}};
			auto& queue = state.event_queues[index];
			++queue.generation;
			queue.key = key;
			queue.protocol = static_cast<std::uint32_t>(ordering);
			queue.capacity = capacity;
			queue.creator = m_session->peer;
		}
		auto& queue = state.event_queues[index];
		if (queue.refs[m_session->slot] == UINT_MAX) return {status::no_space, {}};
		++queue.refs[m_session->slot];
		return {status::ok, event_handle{m_session, index, queue.generation}};
	}

	struct open_result { status code; std::optional<mutex_handle> handle; };
	struct condition_result { status code; std::optional<condition_handle> handle; };

	native_console_sync(const native_console_memory& console, native_console_error_handler error)
		: m_session(std::make_shared<session>(console, error)) {}

private:
	guest_record* own_guest() const
	{
		for (auto& guest : m_session->state.guests)
			if (guest.pid && guest.peer == m_session->peer) return &guest;
		return nullptr;
	}
	static guest_info information(const guest_record& guest)
	{
		return {guest.pid, guest.parent, guest.state, guest.exit_status};
	}
public:
	guest_result register_root_guest()
	{
		if (m_session->host_pid != ::getpid()) return {status::invalid};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken};
		reap(state);
		if (auto* guest = own_guest())
			return {guest->state == guest_state::running && !guest->parent ? status::ok : status::busy, information(*guest)};
		for (const auto& guest : state.guests)
			if (guest.pid && !guest.parent && guest.state == guest_state::running) return {status::exists};
		for (auto& guest : state.guests)
		{
			if (guest.pid) continue;
			if (state.next_guest == INT_MAX) return {status::no_space};
			guest = {++state.next_guest, 0, m_session->peer, guest_state::running, 0, {}};
			return {status::ok, information(guest)};
		}
		return {status::no_space};
	}
	guest_result reserve_guest_child()
	{
		if (m_session->host_pid != ::getpid()) return {status::invalid};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken};
		reap(state);
		const auto* parent = own_guest();
		if (!parent || parent->state != guest_state::running) return {status::not_owner};
		for (auto& guest : state.guests)
		{
			if (guest.pid) continue;
			if (state.next_guest == INT_MAX) return {status::no_space};
			guest_token token{};
			ssize_t count;
			do { count = ::getrandom(&token, sizeof(token), 0); } while (count < 0 && errno == EINTR);
			if (count != sizeof(token) || !(token.words[0] | token.words[1])) return {status::broken};
			guest = {++state.next_guest, parent->pid, 0, guest_state::reserved, 0, token};
			return {status::ok, information(guest), token};
		}
		return {status::no_space};
	}
	guest_result claim_guest_child(guest_token token)
	{
		if (m_session->host_pid != ::getpid() || !(token.words[0] | token.words[1])) return {status::invalid};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken};
		reap(state);
		if (own_guest()) return {status::busy};
		for (auto& guest : state.guests)
		{
			if (!guest.pid || guest.state != guest_state::reserved ||
				guest.token.words[0] != token.words[0] || guest.token.words[1] != token.words[1]) continue;
			bool parent_alive = false;
			for (const auto& parent : state.guests)
				if (parent.pid == guest.parent && parent.state == guest_state::running) parent_alive = true;
			if (!parent_alive) return {status::not_found};
			guest.peer = m_session->peer;
			guest.state = guest_state::running;
			guest.token = {}; // Consumed exactly once, including across separate hosts.
			return {status::ok, information(guest)};
		}
		return {status::not_found};
	}
	guest_result current_guest() const
	{
		if (m_session->host_pid != ::getpid()) return {status::invalid};
		guard lock{m_session->state.control};
		if (!lock) return {status::broken};
		const auto* guest = own_guest();
		return guest ? guest_result{status::ok, information(*guest)} : guest_result{status::not_found};
	}
	guest_result inspect_guest(std::uint32_t pid)
	{
		if (m_session->host_pid != ::getpid()) return {status::invalid};
		guard lock{m_session->state.control};
		if (!lock) return {status::broken};
		reap(m_session->state);
		for (const auto& guest : m_session->state.guests)
			if (guest.pid == pid && pid) return {status::ok, information(guest)};
		return {status::not_found};
	}
	status finish_guest(std::int32_t exit_status)
	{
		if (m_session->host_pid != ::getpid()) return status::invalid;
		guard lock{m_session->state.control};
		if (!lock) return status::broken;
		auto* guest = own_guest();
		if (!guest || guest->state != guest_state::running) return status::not_found;
		guest->state = guest_state::exited;
		guest->exit_status = exit_status;
		return status::ok;
	}
	// Called after an actual waitpid result or a failed host spawn. It never
	// invents a guest exit status from the host process's termination status.
	status child_host_exited(std::uint32_t pid)
	{
		if (m_session->host_pid != ::getpid()) return status::invalid;
		guard lock{m_session->state.control};
		if (!lock) return status::broken;
		const auto* parent = own_guest();
		if (!parent) return status::not_owner;
		for (auto& guest : m_session->state.guests)
		{
			if (guest.pid != pid || !pid) continue;
			if (guest.parent != parent->pid) return status::not_owner;
			if (guest.peer)
			{
				for (std::size_t slot = 0; slot < max_processes; ++slot)
					if (m_session->state.processes[slot].peer == guest.peer) { remove_peer(m_session->state, slot); break; }
			}
			if (guest.state != guest_state::exited) guest.state = guest_state::host_failed;
			guest.token = {};
			return status::ok;
		}
		return status::not_found;
	}
	status reap_guest_child(std::uint32_t pid)
	{
		if (m_session->host_pid != ::getpid()) return status::invalid;
		guard lock{m_session->state.control};
		if (!lock) return status::broken;
		reap(m_session->state);
		const auto* parent = own_guest();
		if (!parent) return status::not_owner;
		for (auto& guest : m_session->state.guests)
		{
			if (guest.pid != pid || !pid) continue;
			if (guest.parent != parent->pid) return status::not_owner;
			if (guest.state == guest_state::reserved || guest.state == guest_state::running) return status::busy;
			guest = {};
			collect_guest_orphans(m_session->state);
			return status::ok;
		}
		return status::not_found;
	}

	struct thread_result { status code; std::uint32_t id = 0; std::uint64_t actor = 0; };
	thread_result allocate_ppu_thread(std::uint32_t slot)
	{
		if (slot >= 100 || m_session->host_pid != ::getpid()) return {status::invalid};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken};
		reap(state);
		const auto* guest = own_guest();
		if (!guest || guest->state != guest_state::running) return {status::not_owner};
		if (state.next_thread_generation[slot] >= 4096) return {status::no_space};
		for (auto& thread : state.threads)
		{
			if (thread.id) continue;
			const auto id = 0x01000000u | (state.next_thread_generation[slot]++ << 12) | slot;
			thread = {id, guest->pid, m_session->peer};
			return {status::ok, id, std::uint64_t{thread.peer} << 32 | id};
		}
		return {status::no_space};
	}
	thread_result resolve_ppu_thread(std::uint32_t id)
	{
		if (m_session->host_pid != ::getpid()) return {status::invalid};
		guard lock{m_session->state.control};
		if (!lock) return {status::broken};
		reap(m_session->state);
		for (const auto& thread : m_session->state.threads)
			if (thread.id == id && id) return {status::ok, id, std::uint64_t{thread.peer} << 32 | id};
		return {status::not_found};
	}
	status release_ppu_thread(std::uint32_t id)
	{
		if (m_session->host_pid != ::getpid()) return status::invalid;
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return status::broken;
		for (auto& thread : state.threads)
		{
			if (thread.id != id || !id) continue;
			if (thread.peer != m_session->peer) return status::not_owner;
			const auto actor = std::uint64_t{thread.peer} << 32 | id;
			for (auto& mutex : state.mutexes)
				if (mutex.owner == actor) { mutex.owner_dead = 1; wake(mutex); }
			thread = {};
			return status::ok;
		}
		return status::not_found;
	}

	std::uint64_t actor_for(std::uint32_t thread) const
	{
		return thread && m_session->host_pid == ::getpid() ? std::uint64_t{m_session->peer} << 32 | thread : 0;
	}

	open_result open_mutex(std::uint64_t key, open_mode mode, protocol ordering, bool recursive)
	{
		if (m_session->host_pid != ::getpid() || !key || mode < open_mode::create || mode > open_mode::open_or_create) return {status::invalid, {}};
		auto& state = m_session->state;
		guard lock{state.control};
		if (!lock) return {status::broken, {}};
		reap(state);
		std::size_t index = max_mutexes;
		for (std::size_t slot = 0; slot < max_mutexes; ++slot)
			if (state.mutexes[slot].key == key) { index = slot; break; }
		if (index != max_mutexes)
		{
			if (mode == open_mode::create) return {status::exists, {}};
		}
		else
		{
			if (mode == open_mode::open) return {status::not_found, {}};
			if (ordering == protocol::priority_inherit) return {status::unsupported, {}};
			if (ordering != protocol::fifo && ordering != protocol::priority) return {status::invalid, {}};
			for (std::size_t slot = 0; slot < max_mutexes; ++slot)
			{
				if (!state.mutexes[slot].key && state.mutexes[slot].generation != UINT_MAX) { index = slot; break; }
			}
			if (index == max_mutexes) return {status::no_space, {}};
			auto& mutex = state.mutexes[index];
			++mutex.generation;
			mutex.protocol = static_cast<std::uint32_t>(ordering);
			mutex.recursive = recursive;
			mutex.key = key;
		}
		auto& mutex = state.mutexes[index];
		if (mutex.refs[m_session->slot] == UINT_MAX) return {status::no_space, {}};
		++mutex.refs[m_session->slot];
		return {status::ok, mutex_handle{m_session, index, mutex.generation}};
	}

	condition_result open_condition(std::uint64_t key, open_mode mode, const mutex_handle& associated)
	{
		if (m_session->host_pid != ::getpid() || !key || mode < open_mode::create || mode > open_mode::open_or_create ||
			associated.m_session->host_pid != ::getpid()) return {status::invalid, {}};
		auto& state = m_session->state;
		const auto& other_identity = associated.m_session->state.control.identity;
		if (state.control.identity[0] != other_identity[0] || state.control.identity[1] != other_identity[1]) return {status::invalid, {}};
		guard lock{state.control};
		if (!lock) return {status::broken, {}};
		reap(state);
		const auto mutex_index = associated.m_index;
		auto& mutex = state.mutexes[mutex_index];
		if (!mutex.key || mutex.generation != associated.m_generation) return {status::not_found, {}};
		std::size_t index = max_conditions;
		for (std::size_t slot = 0; slot < max_conditions; ++slot)
			if (state.conditions[slot].key == key) { index = slot; break; }
		if (index != max_conditions)
		{
			if (mode == open_mode::create) return {status::exists, {}};
			const auto& condition = state.conditions[index];
			if (condition.mutex != mutex_index || condition.mutex_generation != mutex.generation) return {status::invalid, {}};
		}
		else
		{
			if (mode == open_mode::open) return {status::not_found, {}};
			for (std::size_t slot = 0; slot < max_conditions; ++slot)
				if (!state.conditions[slot].key && state.conditions[slot].generation != UINT_MAX) { index = slot; break; }
			if (index == max_conditions || mutex.conditions == UINT_MAX) return {status::no_space, {}};
			auto& condition = state.conditions[index];
			++condition.generation;
			condition.mutex = mutex_index;
			condition.mutex_generation = mutex.generation;
			condition.key = key;
			++mutex.conditions;
		}
		auto& condition = state.conditions[index];
		if (condition.refs[m_session->slot] == UINT_MAX) return {status::no_space, {}};
		++condition.refs[m_session->slot];
		return {status::ok, condition_handle{m_session, index, condition.generation}};
	}
};
}
#endif
