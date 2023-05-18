
#include <mm/mm_low_mem_framework.h>
#include <mm/mm_resource.h>
#include <core/task.h>
#include <core/debug.h>
#include <core/atomic.h>
#include <core/condvar.h>
#include <core/spm.h>
#include <core/procstat.h>
#include <core/mutex.h>
#include <fs/vfs.h>
#include <trustware/system_info.h>
#include <mm/VMAddressSpace.h>
#include <mm/mm_mapper.h>
#include <mm/mm_locking.h>
#include <lib/AutoLock.h>
#include <mm/oom.h>
#include <debug/unwind.h>
#include <core/sbuf.h>

static mutex_t oom_mutex{"oom mutex"};
static time_t last_oom_kill{0};
static bool oom_kill_in_progress;
static mutex_t oom_kill_in_progress_lock{"oom kill in progress"};
static cond_var_t oom_kill_in_progress_cond{"oom kill in progress"};
static uint32_t current_oom_cycle{0};

static constexpr time_t kOomKillGracePeriod = SECONDS(2);

static inline bool team_is_ta(struct task * team) {
	return team->comm_scm > 0;
}

static u_long team_map_count(struct task *team) {
	u_long count = 0;

	VMAddressSpace *addressSpace = VMAddressSpace::Get(team->id);

	if (addressSpace) {
		VMTranslationMap *translationMap = addressSpace->TranslationMap();

		translationMap->Lock();
		count = translationMap->MappedSize();
		translationMap->Unlock();
		addressSpace->Put();
	}

	return count;
}

static u_long team_total_vm(struct task *team) {
	u_long count = 0;

	AddressSpaceReadLocker readLocker(team->id);

	if (!readLocker.IsLocked())
		return 0;

	VMAddressSpace *addressSpace = readLocker.AddressSpace();

	// Iterate over all areas
	auto areaIterator = addressSpace->GetAreaIterator();

	while (areaIterator.HasNext()) {
		auto area = areaIterator.Next();

		// Ignore file mappings as they can go away
		if (area->cache_type != CACHE_TYPE_RAM)
			continue;

		// Include total area size
		count += area->Size() / PAGE_SIZE;
	}

	return count;
}

#if defined(CONFIG_DEBUG)
static const char * team_privilege_name(struct task * team) {
	switch(team->privilege) {
	//								 privilege
	case PRIVILEGE_PUBLIC:   return "PUBLIC  ";
	case PRIVILEGE_PARTNER:  return "PARTNER ";
	case PRIVILEGE_PLATFORM: return "PLATFORM";
	default:				 return "????????";
	}
}

static void dump_task(backtrace_output& out, struct task * team) {
	out.print("[%c%6d]  %s   %5d   %5lu   %8lu  %s\n",
			team == team_get_current_team() ? '*' : ' ',
			team->id,
			team_privilege_name(team),
			team->effective_uid,
			team_map_count(team),
			team_total_vm(team),
			team->name);
}

static void dump_tasks(backtrace_output& out) {
	out.print("Tasks state:\n");
	out.print("[  pid  ]  privilege     uid    rss   total_vm  name\n");

	TeamListIterator iterator;

	while (auto task = iterator.Next()) {
		if (task->id != B_SYSTEM_TEAM) {
			dump_task(out, task);
		}
		task->ReleaseReference();
	}
}
#endif

static u_long team_kill_score(struct task *team) {
	u_long score = 0;

	if (team->state != TEAM_STATE_NORMAL)
		return 0;

	score += team_total_vm(team);
	score += team_map_count(team);

	return score;
}

static u_long team_score_by_uuid(const struct uuid& teamUUID) {
	u_long score = 0;
	TeamListIterator iterator;
	while (auto task = iterator.Next()) {
		// Already have reference from iterator
		BReference<struct task> teamRef(task, true);
		if(uuid_matches(&teamUUID, &task->uuid)) {
			// Mark this task that we've already calculated OOM for
			// so outer loop will skip it
			task->oom_data.oom_cycle = current_oom_cycle;

			score += team_kill_score(task);
		}
	}
	return score;
}

static constexpr int category_count = 4;

static int team_kill_category(struct task * team) {
	// Non-TA apps go last
	if(!team_is_ta(team))
		return 0;
	// Platform TAs
	if(team->privilege == PRIVILEGE_PLATFORM)
		return 1;
	// Partner TAs
	if(team->privilege == PRIVILEGE_PARTNER)
		return 2;
	// Highest priority to kill for public TAs
	return 3;
}

static bool kill_all_by_uuid(struct task * originalTeam, backtrace_output& out)
{
	int killed_count = 0;
	Signal oom_signal(SIGKILL, 0, -ENOMEM, B_SYSTEM_TEAM);
	TeamListIterator iterator;
	auto killUUID = originalTeam->uuid;

	{
		char str[UUID_STR_LEN];
		uuid_to_string(&killUUID, str);
		out << "Kill UUID " << str << "\n";
	}

	while (auto task = iterator.Next()) {
		// Already have reference from iterator
		BReference<struct task> teamRef(task, true);

		if (uuid_matches(&killUUID, &task->uuid)) {
			if (send_signal_to_task(task, oom_signal, B_DO_NOT_RESCHEDULE)
					== 0) {
				out.print("OOM kill PID=%d, UID=%d, GID=%d, NAME=%s\n",
						task->id, task->effective_uid, task->effective_gid,
						task->name);
				++killed_count;
			}
		}
	}

	return killed_count != 0;
}

static bool select_task_to_kill(backtrace_output& out) {
	// Avoid killing tasks too often as we don't want
	// too kill too many tasks
	if(system_time() < last_oom_kill + kOomKillGracePeriod) {
		return false;
	}

	last_oom_kill = system_time();
	current_oom_cycle = std::max<uint32_t>(current_oom_cycle + 1, 1);

	TeamListIterator iterator;

	BReference<struct task> bestTeam[category_count];
	std::array<u_long, category_count> bestScore{};

	while (auto task = iterator.Next()) {
		// Already have reference from iterator
		BReference<struct task> teamRef(task, true);

		// Have we already computed this one ? This is needed to
		// reduce complexity of calculating of UUID RAM usage
		if (task->oom_data.oom_cycle == current_oom_cycle)
			continue;

		task->oom_data.oom_cycle = current_oom_cycle;

		// Skip kernel task
		if (task->id == B_SYSTEM_TEAM)
			continue;

		// Tasks with infinite wait time are not killed
		if (task->flags & TEAM_FLAG_CRITICAL)
			continue;

		int category = team_kill_category(task);
		u_long score;

		if(team_is_ta(task)) {
			score = team_score_by_uuid(task->uuid);
		} else {
			score = team_kill_score(task);
		}

		if (score == 0)
			continue;

		if (bestScore[category] < score) {
			bestScore[category] = score;
			bestTeam[category] = std::move(teamRef);
		}
	}

	/*
	 * Kill public TAs first, platform TAs last
	 */
	for(int i = category_count - 1 ; i >= 0 ; --i) {
		if (bestTeam[i]) {
			struct task *team = bestTeam[i].Get();

			if(team_is_ta(team)) {
				return kill_all_by_uuid(team, out);
			} else {
				// If team is not dying, just kill it
				// Otherwise we're going to wait until task is killed
				if (team->state == TEAM_STATE_NORMAL &&
						!team->pending_signals.AllSignals().IsMember(SIGKILL)) {
					// Disable IRQs so we don't try to allocate
					// the signal to be queued
					InterruptsLocker irqLocker;
					Signal oom_signal(SIGKILL, 0, -ENOMEM, B_SYSTEM_TEAM);
					if (send_signal_to_task(team, oom_signal, B_DO_NOT_RESCHEDULE)
							== 0) {
						irqLocker.Unlock();

						out.print("OOM kill PID=%d, UID=%d, GID=%d, NAME=%s\n",
								team->id, team->effective_uid, team->effective_gid,
								team->name);

						return true;
					}
				}
				return false;
			}
		}
	}

	out.print("Could not find task to kill ?\n");
	return true;
}

static void oom_kill() {
#if defined(CONFIG_DEBUG)
	static char crash_trace_buffer[1024];
	sbuf sb;
	sbuf_new(&sb, crash_trace_buffer, sizeof(crash_trace_buffer),
			SBUF_FIXEDLEN | SBUF_LINECHK);
	sbuf_set_drain(&sb, sbuf_printf_drain, nullptr);
	sbuf_backtrace_output out(sb);
#else
	crash_backtrace_output out;
#endif

	/*
	 * If current task is killed, simply return as we're about
	 * to reclaim memory anyway
	 */
	if (get_current_thread()->task->PendingSignals().IsMember(SIGKILL))
		return;

	if (select_task_to_kill(out)) {
#if defined(CONFIG_DEBUG)
		dump_tasks(out);
#endif
	}

#if defined(CONFIG_DEBUG)
	sbuf_finish(&sb);
#endif
}

void pagefault_oom_kill() {
	// Only one instance of OOM can happen at the same time
	// Other tasks will retry on page fault handler
	if(auto locker = std::unique_lock(oom_mutex, std::try_to_lock) ; locker.owns_lock()) {
		mutex_lock(&oom_kill_in_progress_lock);
		oom_kill_in_progress = true;
		mutex_unlock(&oom_kill_in_progress_lock);

		oom_kill();

		mutex_lock(&oom_kill_in_progress_lock);
		oom_kill_in_progress = false;
		oom_kill_in_progress_cond.NotifyAll();
		mutex_unlock(&oom_kill_in_progress_lock);
	} else {
		auto kill_locker = std::unique_lock(oom_kill_in_progress_lock);
		while (oom_kill_in_progress) {
			cond_var_entry entry;
			oom_kill_in_progress_cond.Add(&entry);
			kill_locker.unlock();
			// Any signal can interrupt this waiting not to
			// increase possible latency
			int error = entry.Wait(B_CAN_INTERRUPT);

			if (error)
				return;

			kill_locker.lock();
		}
	}
}

bool oom_kill_page_alloc_retry(bool killable) {
	if (!oom_mutex.try_lock()) {
		auto kill_locker = std::unique_lock(oom_kill_in_progress_lock);
		while (oom_kill_in_progress) {
			cond_var_entry entry;
			oom_kill_in_progress_cond.Add(&entry);
			kill_locker.unlock();

			// Kill signal can interrupt us, but it will terminate waiting
			int error = entry.Wait(killable ? B_KILL_CAN_INTERRUPT : 0);

			if (error)
				return false;;

			kill_locker.lock();
		}

		return true;
	}

	mutex_lock(&oom_kill_in_progress_lock);
	oom_kill_in_progress = true;
	mutex_unlock(&oom_kill_in_progress_lock);

	oom_kill();

	mutex_lock(&oom_kill_in_progress_lock);
	oom_kill_in_progress = false;
	oom_kill_in_progress_cond.NotifyAll();
	mutex_unlock(&oom_kill_in_progress_lock);

	oom_mutex.unlock();

	// Retry allocation only if we haven't been killed
	return !get_current_thread()->is_interrupted(B_KILL_CAN_INTERRUPT);
}
