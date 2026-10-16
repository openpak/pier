#pragma once

// Linux host backing for the two permanent LV2 system-memory regions. This
// supplies storage only: native firmware must initialize the packet transport.
// Descriptor inheritance, rather than a filesystem name, identifies a console.
#ifdef __linux__

#include "native_console_control.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <cstdlib>
#include <utility>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace pier
{
class native_console_memory
{
public:
	// The embedding runtime supplies its failure mechanism (Pier builds without
	// C++ exception syntax). The callback must not return.
	using error_handler = native_console_error_handler;

	static constexpr std::uint64_t sysutil_key = 0x8006010000000010ULL;
	static constexpr std::uint64_t mio_key = 0x80004d494f323211ULL;
	static constexpr std::size_t region_size = 0x10000;
	static constexpr std::size_t control_size = 4 * region_size;
	static constexpr std::size_t storage_size = 3 * region_size + control_size;

	class mapping
	{
		void* m_address = MAP_FAILED;
		std::size_t m_size;

	public:
		explicit mapping(void* address, std::size_t size = region_size) noexcept : m_address(address), m_size(size) {}
		mapping(const mapping&) = delete;
		mapping& operator=(const mapping&) = delete;
		mapping(mapping&& other) noexcept : m_address(std::exchange(other.m_address, MAP_FAILED)), m_size(other.m_size) {}
		mapping& operator=(mapping&&) = delete;

		~mapping()
		{
			if (m_address != MAP_FAILED)
				::munmap(m_address, m_size);
		}

		std::span<std::byte> bytes() const noexcept
		{
			return std::span<std::byte>{static_cast<std::byte*>(m_address), m_size};
		}
	};

	struct region_descriptor
	{
		int fd;
		std::uint64_t offset;
		std::uint64_t size;
	};

private:
	int m_fd = -1;
	error_handler m_on_error;
	static constexpr int required_seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
	// Native host ABI, versioned and checked before using any shared storage.
	static constexpr std::array<std::uint64_t, 8> header =
	{
		0x504945524c56324dULL, 4, region_size, storage_size,
		sysutil_key, mio_key, 0x0102030405060708ULL, sizeof(native_console_control),
	};

	native_console_memory(int fd, error_handler on_error) noexcept : m_fd(fd), m_on_error(on_error) {}

	[[noreturn]] static void fail(error_handler on_error, const char* operation, int error = errno)
	{
		on_error(operation, error);
		std::abort();
	}

	void validate() const
	{
		struct stat info{};
		if (::fstat(m_fd, &info) < 0)
			fail(m_on_error, "native console memory fstat");
		if (!S_ISREG(info.st_mode) || info.st_size != storage_size)
			fail(m_on_error, "invalid native console memory size/type", EINVAL);
		const int seals = ::fcntl(m_fd, F_GET_SEALS);
		if (seals < 0)
			fail(m_on_error, "native console memory seals");
		if ((seals & required_seals) != required_seals)
			fail(m_on_error, "unsealed native console memory", EINVAL);
		std::array<std::uint64_t, header.size()> actual{};
		ssize_t count;
		do
		{
			count = ::pread(m_fd, actual.data(), sizeof(actual), 0);
		} while (count < 0 && errno == EINTR);
		if (count < 0)
			fail(m_on_error, "native console memory header read");
		if (count != sizeof(actual) || actual != header)
			fail(m_on_error, "incompatible native console memory header", EINVAL);
		auto control_mapping = map_control();
		const auto& control = *reinterpret_cast<const native_console_control*>(control_mapping.bytes().data());
		if (control.magic != native_console_control::magic_value || control.abi_size != sizeof(control))
			fail(m_on_error, "incompatible native console control header", EINVAL);
	}

public:
	native_console_memory(const native_console_memory&) = delete;
	native_console_memory& operator=(const native_console_memory&) = delete;
	native_console_memory(native_console_memory&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)), m_on_error(other.m_on_error) {}
	native_console_memory& operator=(native_console_memory&&) = delete;

	~native_console_memory()
	{
		if (m_fd >= 0)
			::close(m_fd);
	}

	static native_console_memory create(error_handler on_error)
	{
		const int fd = static_cast<int>(::syscall(SYS_memfd_create, "pier-console-lv2", MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (fd < 0)
			fail(on_error, "native console memory create");
		native_console_memory result{fd, on_error};
		if (::fchmod(fd, S_IRUSR | S_IWUSR) < 0 || ::ftruncate(fd, storage_size) < 0)
			fail(on_error, "native console memory allocation");
		{
			auto control = result.map_control();
			initialize_native_console_control(control.bytes().data(), on_error);
		}
		ssize_t count;
		do
		{
			count = ::pwrite(fd, header.data(), sizeof(header), 0);
		} while (count < 0 && errno == EINTR);
		if (count < 0)
			fail(on_error, "native console memory header write");
		if (count != sizeof(header))
			fail(on_error, "short native console memory header write", EIO);
		if (::fcntl(fd, F_ADD_SEALS, required_seals) < 0)
			fail(on_error, "native console memory seal");
		return result;
	}

	// Duplicate a capability explicitly passed by a future console supervisor.
	// Invalid descriptors fail; attaching never creates replacement storage.
	static native_console_memory attach(int borrowed_fd, error_handler on_error)
	{
		const int fd = ::fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0);
		if (fd < 0)
			fail(on_error, "native console memory descriptor duplicate");
		native_console_memory result{fd, on_error};
		result.validate();
		return result;
	}

	region_descriptor region(std::uint64_t key) const
	{
		if (m_fd < 0)
			fail(m_on_error, "closed native console memory", EBADF);
		if (key == sysutil_key)
			return {m_fd, region_size, region_size};
		if (key == mio_key)
			return {m_fd, 2 * region_size, region_size};
		fail(m_on_error, "unknown native console system-memory key", EINVAL);
	}

	mapping map(std::uint64_t key) const
	{
		const auto descriptor = region(key);
		void* address = ::mmap(nullptr, descriptor.size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor.fd, descriptor.offset);
		if (address == MAP_FAILED)
			fail(m_on_error, "native console system-memory mapping");
		return mapping{address};
	}

	// Host kernel metadata is never mapped into a guest address space.
	mapping map_control() const
	{
		void* address = ::mmap(nullptr, control_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 3 * region_size);
		if (address == MAP_FAILED)
			fail(m_on_error, "native console control mapping");
		return mapping{address, control_size};
	}

	// Borrowed descriptor: CLOEXEC stays set until an explicit spawn file action
	// passes it to a child. There is no globally discoverable console name.
	int fd() const noexcept
	{
		return m_fd;
	}
};
}

#endif
