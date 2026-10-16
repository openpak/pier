#include "stdafx.h"

#include "util/serialization.hpp"
#include "Emu/IdManager.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/timers.hpp"

#include "sys_cond.h"
#include "native_console_runtime.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_cond);

#ifdef __linux__
static std::shared_ptr<pier::native_condition> get_native_condition(u32 id)
{
	if (!g_fxo->get<pier::native_console_context>().kernel) return {};
	const auto object = idm::get<lv2_obj, lv2_cond>(id, [](lv2_cond& condition) { return condition.native; });
	return object ? object.ret : nullptr;
}
#endif

lv2_cond::lv2_cond(utils::serial& ar) noexcept
	: key(ar)
	, name(ar)
	, mtx_id(ar)
	, mutex(idm::check_unlocked<lv2_obj, lv2_mutex>(mtx_id))
	, _mutex(idm::get_unlocked<lv2_obj, lv2_mutex>(mtx_id)) // May be nullptr
{
}

lv2_cond::lv2_cond(u64 key, u64 name, u32 mtx_id, shared_ptr<lv2_obj> mutex0) noexcept
	: key(key)
	, name(name)
	, mtx_id(mtx_id)
	, mutex(static_cast<lv2_mutex*>(mutex0.get()))
	, _mutex(mutex0)
{
}

CellError lv2_cond::on_id_create()
{
	exists++;

	static auto do_it = [](lv2_cond* _this) -> CellError
	{
		if (lv2_obj::check(_this->mutex))
		{
			_this->mutex->cond_count++;
			return {};
		}

		// Mutex has been destroyed, cannot create conditional variable
		return CELL_ESRCH;
	};

	if (mutex)
	{
		return do_it(this);
	}

	ensure(!!Emu.DeserialManager());

	Emu.PostponeInitCode([this]()
	{
		if (!mutex)
		{
			_mutex = static_cast<shared_ptr<lv2_obj>>(ensure(idm::get_unlocked<lv2_obj, lv2_mutex>(mtx_id)));
			mutex = static_cast<lv2_mutex*>(_mutex.get());
		}

		// Defer function
		ensure(CellError{} == do_it(this));
	});

	return {};
}

std::function<void(void*)> lv2_cond::load(utils::serial& ar)
{
#ifdef __linux__
	if (g_fxo->get<pier::native_console_context>().kernel)
		fmt::throw_exception("Native console condition savestate restoration is not supported");
#endif
	return load_func(make_shared<lv2_cond>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_cond::save(utils::serial& ar)
{
	if (native) fmt::throw_exception("Native console condition savestates are not supported");
	ar(key, name, mtx_id);
}

error_code sys_cond_create(ppu_thread& ppu, vm::ptr<u32> cond_id, u32 mutex_id, vm::ptr<sys_cond_attribute_t> attr)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_create(cond_id=*0x%x, mutex_id=0x%x, attr=*0x%x)", cond_id, mutex_id, attr);
	if (!cond_id || !attr) return CELL_EFAULT;

	std::shared_ptr<pier::native_mutex> native_mutex;
	auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id, [&](lv2_mutex& value) { native_mutex = value.native; });

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	const auto _attr = *attr;

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (ipc_key)
	{
		sys_cond.warning("sys_cond_create(cond_id=*0x%x, attr=*0x%x): IPC=0x%016x", cond_id, attr, ipc_key);
	}

	std::shared_ptr<pier::native_condition> native;
	s32 local_flags = _attr.flags;
#ifdef __linux__
	if (native_mutex)
	{
		if (!ipc_key) return CELL_ENOSYS;
		auto& kernel = g_fxo->get<pier::native_console_context>().kernel;
		auto result = kernel->open_condition(ipc_key, static_cast<pier::native_console_sync::open_mode>(+_attr.flags), native_mutex->handle);
		if (result.code != pier::native_console_sync::status::ok) return pier::native_console_error(result.code);
		native = std::make_shared<pier::native_condition>(native_mutex, std::move(*result.handle));
		local_flags = SYS_SYNC_NOT_CARE;
		sys_cond.notice("Native console condition attached: IPC=0x%016x", ipc_key);
	}
#endif
	if (const auto error = lv2_obj::create<lv2_cond>(_attr.pshared, ipc_key, local_flags, [&]
	{
		auto result = make_single<lv2_cond>(
			ipc_key,
			_attr.name_u64,
			mutex_id,
			std::move(mutex));
		result->native = native;
		return result;
	}))
	{
		return error;
	}

	ppu.check_state();
	*cond_id = idm::last_id<lv2_cond>();
	return CELL_OK;
}

error_code sys_cond_destroy(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_destroy(cond_id=0x%x)", cond_id);

	const auto cond = idm::withdraw<lv2_obj, lv2_cond>(cond_id, [&](lv2_cond& cond) -> CellError
	{
		std::lock_guard lock(cond.mutex->mutex);

#ifdef __linux__
		if (cond.native)
		{
			const auto state = cond.native->handle.idle();
			if (state != pier::native_console_sync::status::ok) return pier::native_console_error(state);
			if (cond.native.use_count() > 1) return CELL_EBUSY;
		}
#endif

		if (atomic_storage<ppu_thread*>::load(cond.sq))
		{
			return CELL_EBUSY;
		}

		cond.mutex->cond_count--;
		lv2_obj::on_id_destroy(cond, cond.key);
		return {};
	});

	if (!cond)
	{
		return CELL_ESRCH;
	}

	if (cond->key)
	{
		sys_cond.warning("sys_cond_destroy(cond_id=0x%x): IPC=0x%016x", cond_id, cond->key);
	}

	if (cond.ret)
	{
		return cond.ret;
	}

	return CELL_OK;
}

error_code sys_cond_signal(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal(cond_id=0x%x)", cond_id);

#ifdef __linux__
	if (auto native = get_native_condition(cond_id)) return pier::native_console_error(native->handle.signal());
#endif

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		const auto cond = idm::check<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				std::lock_guard lock(cond.mutex->mutex);

				if (ppu.state & cpu_flag::suspend)
				{
					// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
					finished = false;
					return;
				}

				if (const auto cpu = cond.schedule<ppu_thread>(cond.sq, cond.mutex->protocol))
				{
					if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
					{
						ppu.state += cpu_flag::again;
						return;
					}

					// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?

					if (cond.mutex->try_own(*cpu))
					{
						cond.awake(cpu);
					}
				}
			}
			else
			{
				cond.mutex->mutex.lock_unlock();

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
				}
			}
		});

		if (!finished)
		{
			continue;
		}

		if (!cond)
		{
			return CELL_ESRCH;
		}

		return CELL_OK;
	}
}

error_code sys_cond_signal_all(ppu_thread& ppu, u32 cond_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal_all(cond_id=0x%x)", cond_id);

#ifdef __linux__
	if (auto native = get_native_condition(cond_id)) return pier::native_console_error(native->handle.signal(true));
#endif

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		const auto cond = idm::check<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				std::lock_guard lock(cond.mutex->mutex);

				if (ppu.state & cpu_flag::suspend)
				{
					// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
					finished = false;
					return;
				}

				for (auto cpu = +cond.sq; cpu; cpu = cpu->next_cpu)
				{
					if (cpu->state & cpu_flag::again)
					{
						ppu.state += cpu_flag::again;
						return;
					}
				}

				cpu_thread* result = nullptr;
				auto sq = cond.sq;
				atomic_storage<ppu_thread*>::release(cond.sq, nullptr);

				while (const auto cpu = cond.schedule<ppu_thread>(sq, SYS_SYNC_PRIORITY))
				{
					if (cond.mutex->try_own(*cpu))
					{
						ensure(!std::exchange(result, cpu));
					}
				}

				if (result)
				{
					cond.awake(result);
				}
			}
			else
			{
				cond.mutex->mutex.lock_unlock();

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
				}
			}
		});

		if (!finished)
		{
			continue;
		}

		if (!cond)
		{
			return CELL_ESRCH;
		}

		return CELL_OK;
	}
}

error_code sys_cond_signal_to(ppu_thread& ppu, u32 cond_id, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_signal_to(cond_id=0x%x, thread_id=0x%x)", cond_id, thread_id);

#ifdef __linux__
	if (auto native = get_native_condition(cond_id))
	{
		const auto target = g_fxo->get<pier::native_console_context>().kernel->resolve_ppu_thread(thread_id);
		if (target.code != pier::native_console_sync::status::ok) return pier::native_console_error(target.code);
		const auto result = native->handle.signal(false, target.actor);
		// A real target thread that is not waiting on this condition is EPERM;
		// a missing public thread ID was already rejected above as ESRCH.
		if (result == pier::native_console_sync::status::not_found) return CELL_EPERM;
		return pier::native_console_error(result);
	}
#endif

	const u64 start_time = get_system_time();
	bool has_lower_prio_or_is_the_mutex_owner = false;

	while (true)
	{
		if (ppu.test_stopped())
		{
			ppu.state += cpu_flag::again;
			return {};
		}

		bool finished = true;

		ppu.state += cpu_flag::wait;

		const auto cond = idm::check<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond)
		{
			const auto target_ppu = idm::check_unlocked<named_thread<ppu_thread>>(thread_id);

			if (!target_ppu)
			{
				return -1;
			}

			if (!has_lower_prio_or_is_the_mutex_owner && cond.mutex->control.load().owner != ppu.id)
			{
				if (cond.mutex->control.load().owner == thread_id)
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
				else if (target_ppu->is_lower_priority_than(ppu))
				{
					has_lower_prio_or_is_the_mutex_owner = true;
				}
			}

			if (atomic_storage<ppu_thread*>::load(cond.sq))
			{
				std::lock_guard lock(cond.mutex->mutex);

				if (ppu.state & cpu_flag::suspend)
				{
					// Test if another signal caused the current thread to be suspended, in which case it needs to wait until the thread wakes up (otherwise the signal may cause unexpected results)
					finished = false;
					return 0;
				}

				for (auto cpu = +cond.sq; cpu; cpu = cpu->next_cpu)
				{
					if (cpu->id == thread_id)
					{
						if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
						{
							ppu.state += cpu_flag::again;
							return 0;
						}

						ensure(cond.unqueue(cond.sq, cpu));

						if (cond.mutex->try_own(*cpu))
						{
							cond.awake(cpu);
						}

						return 1;
					}
				}
			}
			else
			{
				cond.mutex->mutex.lock_unlock();

				if (ppu.state & cpu_flag::suspend)
				{
					finished = false;
					return 0;
				}
			}

			return 0;
		});

		if (!finished)
		{
			continue;
		}

		if (!cond || cond.ret == -1)
		{
			return CELL_ESRCH;
		}

		if (!cond.ret)
		{
			if (has_lower_prio_or_is_the_mutex_owner && get_system_time() - start_time < 50)
			{
				// RPCS3 is not cycle accurate
				// Threads with higher (lower) priority get timing benefits that we cannot provide accurately
				// Give the thread another chance
				busy_wait(5000);
				continue;
			}

			return { CELL_EPERM, thread_id };
		}

		return CELL_OK;
	}
}

error_code sys_cond_wait(ppu_thread& ppu, u32 cond_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_cond.trace("sys_cond_wait(cond_id=0x%x, timeout=%lld)", cond_id, timeout);

#ifdef __linux__
	if (auto native = get_native_condition(cond_id))
	{
		lv2_obj::sleep(ppu);
		const auto result = native->handle.wait(ppu.id, ppu.prio.load().prio, timeout, [&]
		{
			return is_stopped(+ppu.state);
		});
		if (result == pier::native_console_sync::status::cancelled)
		{
			ppu.state += cpu_flag::again;
			return {};
		}
		lv2_obj::awake(&ppu);
		ppu.state -= cpu_flag::signal;
		ppu.check_state();
		return pier::native_console_error(result);
	}
#endif

	// Further function result
	ppu.gpr[3] = CELL_OK;

	auto& sstate = *ppu.optional_savestate_state;

	const auto cond = idm::get<lv2_obj, lv2_cond>(cond_id, [&, notify = lv2_obj::notify_all_t()](lv2_cond& cond) -> s64
	{
		if (!ppu.loaded_from_savestate && atomic_storage<u32>::load(cond.mutex->control.raw().owner) != ppu.id)
		{
			return -1;
		}

		lv2_obj::prepare_for_sleep(ppu);

		std::lock_guard lock(cond.mutex->mutex);

		const u64 syscall_state = sstate.try_read<u64>().second;
		sstate.clear();

		if (ppu.loaded_from_savestate)
		{
			if (syscall_state & 1)
			{
				// Mutex sleep
				ensure(!cond.mutex->try_own(ppu));
			}
			else
			{
				lv2_obj::emplace(cond.sq, &ppu);
			}

			cond.sleep(ppu, timeout);
			return static_cast<u32>(syscall_state >> 32);
		}

		// Register waiter
		lv2_obj::emplace(cond.sq, &ppu);

		// Unlock the mutex
		const u32 count = cond.mutex->lock_count.exchange(0);

		if (const auto cpu = cond.mutex->reown<ppu_thread>())
		{
			if (cpu->state & cpu_flag::again)
			{
				ensure(cond.unqueue(cond.sq, &ppu));
				ppu.state += cpu_flag::again;
				return 0;
			}

			cond.mutex->append(cpu);
		}

		// Sleep current thread and schedule mutex waiter
		cond.sleep(ppu, timeout);

		// Save the recursive value
		return count;
	});

	if (!cond)
	{
		return CELL_ESRCH;
	}

	if (ppu.state & cpu_flag::again)
	{
		return {};
	}

	if (cond.ret < 0)
	{
		return CELL_EPERM;
	}

	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			std::lock_guard lock(cond->mutex->mutex);

			bool mutex_sleep = false;
			bool cond_sleep = false;

			for (auto cpu = atomic_storage<ppu_thread*>::load(cond->sq); cpu; cpu = cpu->next_cpu)
			{
				if (cpu == &ppu)
				{
					cond_sleep = true;
					break;
				}
			}

			for (auto cpu = atomic_storage<ppu_thread*>::load(cond->mutex->control.raw().sq); cpu; cpu = cpu->next_cpu)
			{
				if (cpu == &ppu)
				{
					mutex_sleep = true;
					break;
				}
			}

			if (!cond_sleep && !mutex_sleep)
			{
				break;
			}

			const u64 optional_syscall_state = u32{mutex_sleep} | (u64{static_cast<u32>(cond.ret)} << 32);
			sstate(optional_syscall_state);

			ppu.state += cpu_flag::again;
			return {};
		}

		for (usz i = 0; cpu_flag::signal - ppu.state && i < 50; i++)
		{
			busy_wait(500);
		}

		if (ppu.state & cpu_flag::signal)
 		{
			continue;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				const u64 start_time = ppu.start_time;

				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				ppu.state += cpu_flag::wait;

				std::lock_guard lock(cond->mutex->mutex);

				// Try to cancel the waiting
				if (cond->unqueue(cond->sq, &ppu))
				{
					// TODO: Is EBUSY returned after reqeueing, on sys_cond_destroy?
					ppu.gpr[3] = CELL_ETIMEDOUT;

					// Own or requeue
					if (cond->mutex->try_own(ppu))
					{
						break;
					}
				}
				else if (atomic_storage<u32>::load(cond->mutex->control.raw().owner) == ppu.id)
				{
					break;
				}

				cond->mutex->sleep(ppu);
				ppu.start_time = start_time; // Restore start time because awake has been called
				timeout = 0;
				continue;
			}
		}
		else
		{
			ppu.state.wait(state);
		}
	}

	// Verify ownership
	ensure(atomic_storage<u32>::load(cond->mutex->control.raw().owner) == ppu.id);

	// Restore the recursive value
	cond->mutex->lock_count.release(static_cast<u32>(cond.ret));

	return not_an_error(ppu.gpr[3]);
}
