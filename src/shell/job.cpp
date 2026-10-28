#include "shell/job.h"

#include <windows.h>

#include "diag/log.h"

namespace moo::shell {
namespace {
HANDLE g_job = nullptr;
} // namespace

void ApplyProcessTuning() {
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        moo::diag::Write(L"tuning: SetPriorityClass failed (%lu)", GetLastError());
    }

    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limits.BasicLimitInformation.PriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;

        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            moo::diag::Write(L"tuning: SetInformationJobObject failed (%lu)", GetLastError());
        }
        if (!AssignProcessToJobObject(g_job, GetCurrentProcess())) {
            moo::diag::Write(L"tuning: AssignProcessToJobObject failed (%lu)", GetLastError());
        }
    } else {
        moo::diag::Write(L"tuning: CreateJobObject failed (%lu)", GetLastError());
    }

    // Opt out of EcoQoS (Windows 10 1709+). The Chromium feature flag does the
    // same for its child processes; this covers the host.
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    power.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power,
                               sizeof(power))) {
        moo::diag::Write(L"tuning: SetProcessInformation(PowerThrottling) failed (%lu)",
                         GetLastError());
    }

    moo::diag::Write(L"tuning: ABOVE_NORMAL job + EcoQoS opt-out applied");
}

} // namespace moo::shell
