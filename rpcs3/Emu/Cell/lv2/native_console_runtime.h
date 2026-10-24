#pragma once

#ifdef __linux__
#include "native_console_child.h"
#include "Emu/Cell/ErrorCodes.h"

// True when this host boots the firmware (dev_flash vsh.self); defined in System.cpp.
bool pier_boots_firmware();

namespace pier
{
struct native_mutex
{
	native_console_sync::mutex_handle handle;
	explicit native_mutex(native_console_sync::mutex_handle&& value) : handle(std::move(value)) {}
};

struct native_condition
{
	std::shared_ptr<native_mutex> mutex;
	native_console_sync::condition_handle handle;
	native_condition(std::shared_ptr<native_mutex> owner, native_console_sync::condition_handle&& value)
		: mutex(std::move(owner)), handle(std::move(value)) {}
};

struct native_event
{
	native_console_sync::event_handle handle;
	explicit native_event(native_console_sync::event_handle&& value) : handle(std::move(value)) {}
};

struct native_console_context
{
	std::optional<native_console_memory> memory;
	std::unique_ptr<native_console_sync> kernel;
	std::uint32_t guest_pid = 0;
	std::uint32_t parent_pid = 0;
	std::unique_ptr<native_child_supervisor> children; // Destroyed first: stops owned children.

	[[noreturn]] static void fail(const char* operation, int error)
	{
		fmt::throw_exception("%s (errno=%d)", operation, error);
	}

	native_console_context()
	{
		const char* value = std::getenv("PIER_NATIVE_CONSOLE_FD");
		if (!value || !*value)
		{
			// Every firmware boot owns a console of its own, and games it launches
			// join it as child hosts: on Linux this is the only XMB launch path.
			if (!pier_boots_firmware()) return;
			memory.emplace(native_console_memory::create(&fail));
		}
		else
		{
			const std::string_view text{value};
			int fd = -1;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), fd);
			if (error != std::errc{} || end != text.data() + text.size() || fd < 0)
				fmt::throw_exception("Invalid PIER_NATIVE_CONSOLE_FD descriptor");
			memory.emplace(native_console_memory::attach(fd, &fail));
		}
		kernel = std::make_unique<native_console_sync>(*memory, &fail);
		native_console_sync::guest_result registered{};
		if (const char* token_text = std::getenv("PIER_NATIVE_GUEST_TOKEN"))
		{
			const auto token = native_guest_token_parse(token_text);
			const char* parent_text = std::getenv("PIER_NATIVE_PARENT_HOST");
			int parent = 0;
			if (!token || !parent_text) fail("Invalid native guest child capability", EINVAL);
			const std::string_view parent_value{parent_text};
			const auto [last, result] = std::from_chars(parent_value.data(), parent_value.data() + parent_value.size(), parent);
			if (result != std::errc{} || last != parent_value.data() + parent_value.size() || parent <= 0)
				fail("Invalid native guest parent host", EINVAL);
			if (const int error = native_guest_parent_death(parent)) fail("Native child parent-death setup", error);
			registered = kernel->claim_guest_child(*token);
		}
		else
		{
			if (std::getenv("PIER_NATIVE_PARENT_HOST")) fail("Native guest child token missing", EINVAL);
			registered = kernel->register_root_guest();
		}
		if (registered.code != native_console_sync::status::ok) fail("Native guest registration failed", EINVAL);
		guest_pid = registered.info.pid;
		parent_pid = registered.info.parent;
		children = std::make_unique<native_child_supervisor>(*kernel, *memory);
	}
};

inline CellError native_console_error(native_console_sync::status result)
{
	using status = native_console_sync::status;
	switch (result)
	{
	case status::ok: return {};
	case status::not_found: return CELL_ESRCH;
	case status::exists: return CELL_EEXIST;
	case status::invalid: return CELL_EINVAL;
	case status::busy: return CELL_EBUSY;
	case status::deadlock: return CELL_EDEADLK;
	case status::not_owner: return CELL_EPERM;
	case status::overflow: return CELL_EKRESOURCE;
	case status::timed_out: return CELL_ETIMEDOUT;
	case status::cancelled: return CELL_ECANCELED;
	case status::owner_dead: return CELL_EABORT;
	case status::no_space: return CELL_EAGAIN;
	case status::unsupported: return CELL_ENOSYS;
	case status::broken: return CELL_EABORT;
	case status::has_conditions: return CELL_EPERM;
	}
	return CELL_EABORT;
}
}
#endif
