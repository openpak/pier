#pragma once
#ifdef __linux__
#include "native_console_sync.h"
#include <spawn.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace pier
{
inline int native_guest_parent_death(pid_t expected_parent)
{
	if (expected_parent <= 0) return EINVAL;
	if (::prctl(PR_SET_PDEATHSIG, SIGTERM) < 0) return errno;
	// Close the race where the parent died before PR_SET_PDEATHSIG.
	return ::getppid() == expected_parent ? 0 : ESRCH;
}
inline std::string native_guest_token_string(native_console_sync::guest_token token)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string value(32, '0');
	for (unsigned word = 0; word < 2; ++word)
		for (unsigned nibble = 0; nibble < 16; ++nibble)
			value[word * 16 + nibble] = digits[(token.words[word] >> ((15 - nibble) * 4)) & 15];
	return value;
}
inline std::optional<native_console_sync::guest_token> native_guest_token_parse(std::string_view text)
{
	if (text.size() != 32) return {};
	native_console_sync::guest_token token{};
	for (unsigned word = 0; word < 2; ++word)
	{
		const auto* begin = text.data() + word * 16;
		const auto [end, error] = std::from_chars(begin, begin + 16, token.words[word], 16);
		if (error != std::errc{} || end != begin + 16) return {};
	}
	if (!(token.words[0] | token.words[1])) return {};
	return token;
}

// An explicitly owned host child. It does not dispatch firmware syscalls, claim
// game boot success, replace VSH, or share a renderer/input device.
class native_console_child
{
	native_console_sync& m_kernel;
	const pid_t m_owner = ::getpid();
	pid_t m_host;
	std::uint32_t m_guest;
	bool m_reaped = false;
	int m_wait_status = 0;
	native_console_child(native_console_sync& kernel, pid_t host, std::uint32_t guest)
		: m_kernel(kernel), m_host(host), m_guest(guest) {}
public:
	native_console_child(const native_console_child&) = delete;
	native_console_child& operator=(const native_console_child&) = delete;
	~native_console_child()
	{
		if (m_owner != ::getpid()) return;
		stop();
		if (m_reaped) m_kernel.reap_guest_child(m_guest);
	}
	struct result { int error; std::unique_ptr<native_console_child> child; };
	static result spawn(native_console_sync& kernel, const native_console_memory& memory,
		const std::string& executable, const std::vector<std::string>& arguments, char* const* inherited_environment)
	{
		auto reserved = kernel.reserve_guest_child();
		if (reserved.code != native_console_sync::status::ok) return {EAGAIN, {}};
		auto failed = [&](int error)
		{
			kernel.child_host_exited(reserved.info.pid);
			kernel.reap_guest_child(reserved.info.pid);
			return result{error, {}};
		};
		std::vector<std::string> environment;
		for (auto entry = inherited_environment; entry && *entry; ++entry)
		{
			std::string_view text{*entry};
			if (!text.starts_with("PIER_NATIVE_CONSOLE_FD=") && !text.starts_with("PIER_NATIVE_GUEST_TOKEN=") &&
				!text.starts_with("PIER_NATIVE_PARENT_HOST=")) environment.emplace_back(text);
		}
		environment.emplace_back("PIER_NATIVE_CONSOLE_FD=198");
		environment.emplace_back("PIER_NATIVE_GUEST_TOKEN=" + native_guest_token_string(reserved.token));
		environment.emplace_back("PIER_NATIVE_PARENT_HOST=" + std::to_string(::getpid()));
		std::vector<char*> env;
		for (auto& value : environment) env.push_back(value.data());
		env.push_back(nullptr);
		std::vector<std::string> argv{executable};
		argv.insert(argv.end(), arguments.begin(), arguments.end());
		std::vector<char*> args;
		for (auto& value : argv) args.push_back(value.data());
		args.push_back(nullptr);
		posix_spawn_file_actions_t actions;
		int error = ::posix_spawn_file_actions_init(&actions);
		if (error) return failed(error);
		error = ::posix_spawn_file_actions_adddup2(&actions, memory.fd(), 198);
		pid_t host = -1;
		if (!error) error = ::posix_spawn(&host, executable.c_str(), &actions, nullptr, args.data(), env.data());
		::posix_spawn_file_actions_destroy(&actions);
		if (error) return failed(error);
		return {0, std::unique_ptr<native_console_child>{new native_console_child{kernel, host, reserved.info.pid}}};
	}
	// Only this owner may wait/reap this PID. A host wait status is separate
	// from the guest's explicitly reported exit code.
	int poll()
	{
		if (m_owner != ::getpid()) return EINVAL;
		if (m_reaped) return 0;
		pid_t result;
		do { result = ::waitpid(m_host, &m_wait_status, WNOHANG); } while (result < 0 && errno == EINTR);
		if (!result) return EAGAIN;
		if (result < 0) return errno;
		m_reaped = true;
		m_kernel.child_host_exited(m_guest);
		return 0;
	}
	void stop()
	{
		if (m_owner != ::getpid() || !poll()) return;
		// A child reaped elsewhere is no longer ours to signal (PID reuse).
		if (poll() != EAGAIN) return;
		::kill(m_host, SIGTERM);
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < end)
		{
			if (poll() != EAGAIN) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		::kill(m_host, SIGKILL);
		pid_t result;
		do { result = ::waitpid(m_host, &m_wait_status, 0); } while (result < 0 && errno == EINTR);
		if (result == m_host) { m_reaped = true; m_kernel.child_host_exited(m_guest); }
	}
	std::uint32_t guest_pid() const { return m_guest; }
	pid_t host_pid() const { return m_host; }
	std::optional<int> host_wait_status() const { return m_reaped ? std::optional<int>{m_wait_status} : std::nullopt; }
};

// Spawns and waits for guest children on one long-lived thread: Linux
// parent-death signaling follows the creating thread, and only this thread
// calls waitpid. Destruction terminates every child still owned here.
class native_child_supervisor
{
	native_console_sync& m_kernel;
	const native_console_memory& m_memory;
	std::mutex m_lock;
	std::condition_variable m_changed;
	struct request
	{
		std::string executable;
		std::vector<std::string> arguments, environment;
		int error = -1;
		std::uint32_t pid = 0;
	};
	std::vector<request*> m_requests;
	std::vector<std::unique_ptr<native_console_child>> m_children;
	bool m_stop = false;
	std::thread m_thread{[this] { run(); }};

	void run()
	{
		std::unique_lock lock{m_lock};
		while (!m_stop)
		{
			for (auto* entry : std::exchange(m_requests, {}))
			{
				std::vector<char*> environment;
				for (auto& value : entry->environment) environment.push_back(value.data());
				environment.push_back(nullptr);
				auto spawned = native_console_child::spawn(m_kernel, m_memory, entry->executable, entry->arguments, environment.data());
				entry->error = spawned.error;
				if (spawned.child) entry->pid = spawned.child->guest_pid(), m_children.push_back(std::move(spawned.child));
			}
			bool exited = false;
			for (auto& child : m_children)
				if (!child->host_wait_status() && child->poll() == 0) exited = true;
			m_changed.notify_all();
			// ponytail: 50 ms waitpid poll; a pidfd would wake immediately if latency matters.
			m_changed.wait_for(lock, std::chrono::milliseconds(exited ? 0 : 50));
		}
	}
public:
	native_child_supervisor(native_console_sync& kernel, const native_console_memory& memory) : m_kernel(kernel), m_memory(memory) {}
	native_child_supervisor(const native_child_supervisor&) = delete;
	native_child_supervisor& operator=(const native_child_supervisor&) = delete;
	~native_child_supervisor()
	{
		{ std::lock_guard lock{m_lock}; m_stop = true; }
		m_changed.notify_all();
		m_thread.join();
		m_children.clear();
	}
	struct spawn_result { int error; std::uint32_t pid; };
	spawn_result spawn(std::string executable, std::vector<std::string> arguments, std::vector<std::string> environment)
	{
		request entry{std::move(executable), std::move(arguments), std::move(environment)};
		std::unique_lock lock{m_lock};
		m_requests.push_back(&entry);
		m_changed.notify_all();
		m_changed.wait(lock, [&] { return entry.error >= 0 || m_stop; });
		if (entry.error < 0) std::erase(m_requests, &entry);
		return {entry.error < 0 ? ECANCELED : entry.error, entry.pid};
	}
	std::size_t running()
	{
		std::lock_guard lock{m_lock};
		return std::count_if(m_children.begin(), m_children.end(), [](const auto& child) { return !child->host_wait_status(); });
	}
	// Blocks until the host of an owned child is reaped, then releases the
	// guest record. ESRCH: not our child; ECANCELED: stop() returned true.
	struct wait_result { int error; native_console_sync::guest_info info; int host_status; };
	template <typename Stop>
	wait_result wait(std::uint32_t pid, Stop&& stop)
	{
		std::unique_lock lock{m_lock};
		for (;;)
		{
			const auto found = std::find_if(m_children.begin(), m_children.end(), [&](const auto& child) { return child->guest_pid() == pid; });
			if (found == m_children.end()) return {ESRCH, {}, 0};
			if (const auto status = (*found)->host_wait_status())
			{
				const auto guest = m_kernel.inspect_guest(pid);
				m_children.erase(found); // Reaps the guest record.
				return {0, guest.info, *status};
			}
			if (stop()) return {ECANCELED, {}, 0};
			m_changed.wait_for(lock, std::chrono::milliseconds(50));
		}
	}
};
}
#endif
