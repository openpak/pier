#pragma once

#ifdef __linux__
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <pthread.h>
#include <sys/random.h>

namespace pier
{
using native_console_error_handler = void (*)(const char*, int);

struct alignas(64) native_console_control
{
	static constexpr std::uint64_t magic_value = 0x504945524b45524eULL;
	std::uint64_t magic;
	std::uint64_t identity[2];
	std::uint32_t poisoned;
	std::uint32_t abi_size;
	pthread_mutex_t lock;
};

inline void initialize_native_console_control(void* address, native_console_error_handler error)
{
	auto& control = *::new (address) native_console_control{};
	auto check = [&](int result)
	{
		if (result)
		{
			error("native console control initialization", result);
			std::abort();
		}
	};
	ssize_t count;
	do { count = ::getrandom(control.identity, sizeof(control.identity), 0); }
	while (count < 0 && errno == EINTR);
	if (count != sizeof(control.identity))
	{
		error("native console identity initialization", count < 0 ? errno : EIO);
		std::abort();
	}
	pthread_mutexattr_t attr;
	check(::pthread_mutexattr_init(&attr));
	check(::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));
	check(::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST));
	check(::pthread_mutex_init(&control.lock, &attr));
	check(::pthread_mutexattr_destroy(&attr));
	control.abi_size = sizeof(native_console_control);
	control.magic = native_console_control::magic_value;
}
}
#endif
