#include "process.h"

#include "dbg.h"
#include "str.h"
#include "windows.h"

#if defined(CONF_FAMILY_UNIX)
#include <sys/resource.h> // setpriority
#include <sys/wait.h> // waitpid
#include <unistd.h> // _exit, fork, getpid

#include <csignal> // kill
#include <cstdlib> // malloc, free
#elif defined(CONF_FAMILY_WINDOWS)
#include "mem.h"

#include <objbase.h> // required for shellapi.h
#include <process.h> // _getpid
#include <shellapi.h> // ShellExecuteExW
#include <tlhelp32.h> // CreateToolhelp32Snapshot, Thread32First
#include <windows.h> // CloseHandle, GetExitCodeProcess, TerminateProcess

#include <cfenv> // fesetenv
#include <cstring> // std::wstring
#else
#error NOT IMPLEMENTED
#endif

int process_id()
{
#if defined(CONF_FAMILY_WINDOWS)
	return _getpid();
#else
	return getpid();
#endif
}

#if !defined(CONF_PLATFORM_ANDROID)
PROCESS process_execute(const char *file, EShellExecuteWindowState window_state, const char **arguments, const size_t num_arguments)
{
	dbg_assert((arguments == nullptr) == (num_arguments == 0), "Invalid number of arguments");
#if defined(CONF_FAMILY_WINDOWS)
	dbg_assert(str_endswith_nocase(file, ".bat") == nullptr && str_endswith_nocase(file, ".cmd") == nullptr, "Running batch files not allowed");
	dbg_assert(str_endswith(file, ".exe") != nullptr || num_arguments == 0, "Arguments only allowed with .exe files");

	const std::wstring wide_file = windows_utf8_to_wide(file);
	std::wstring wide_arguments = windows_args_to_wide(arguments, num_arguments);

	SHELLEXECUTEINFOW info;
	mem_zero(&info, sizeof(SHELLEXECUTEINFOW));
	info.cbSize = sizeof(SHELLEXECUTEINFOW);
	info.lpVerb = L"open";
	info.lpFile = wide_file.c_str();
	info.lpParameters = num_arguments > 0 ? wide_arguments.c_str() : nullptr;
	switch(window_state)
	{
	case EShellExecuteWindowState::FOREGROUND:
		info.nShow = SW_SHOW;
		break;
	case EShellExecuteWindowState::BACKGROUND:
		info.nShow = SW_SHOWNOACTIVATE;
		break;
	default:
		dbg_assert_failed("Invalid window_state: %d", static_cast<int>(window_state));
	}
	info.fMask = SEE_MASK_NOCLOSEPROCESS;
	// Save and restore the FPU control word because ShellExecute might change it
	fenv_t floating_point_environment;
	int fegetenv_result = fegetenv(&floating_point_environment);
	ShellExecuteExW(&info);
	if(fegetenv_result == 0)
		fesetenv(&floating_point_environment);
	return info.hProcess;
#elif defined(CONF_FAMILY_UNIX)
	char **argv = (char **)malloc((num_arguments + 2) * sizeof(*argv));
	pid_t pid;
	argv[0] = (char *)file;
	for(size_t i = 0; i < num_arguments; ++i)
	{
		argv[i + 1] = (char *)arguments[i];
	}
	argv[num_arguments + 1] = NULL;
	pid = fork();
	if(pid == -1)
	{
		free(argv);
		return 0;
	}
	if(pid == 0)
	{
		execvp(file, argv);
		_exit(1);
	}
	free(argv);
	return pid;
#endif
}

bool process_set_background_priority(PROCESS process)
{
	if(process == INVALID_PROCESS)
		return false;
#if defined(CONF_FAMILY_WINDOWS)
	return SetPriorityClass(process, IDLE_PRIORITY_CLASS) != 0;
#elif defined(CONF_FAMILY_UNIX)
	return setpriority(PRIO_PROCESS, process, 15) == 0;
#endif
}

bool process_set_paused(PROCESS process, bool paused)
{
	if(process == INVALID_PROCESS)
		return false;
#if defined(CONF_FAMILY_WINDOWS)
	const DWORD ProcessId = GetProcessId(process);
	if(ProcessId == 0)
		return false;
	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if(Snapshot == INVALID_HANDLE_VALUE)
		return false;
	bool ChangedThread = false;
	THREADENTRY32 Entry{};
	Entry.dwSize = sizeof(Entry);
	if(Thread32First(Snapshot, &Entry))
	{
		do
		{
			if(Entry.th32OwnerProcessID != ProcessId)
				continue;
			const HANDLE Thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, Entry.th32ThreadID);
			if(!Thread)
				continue;
			const DWORD Result = paused ? SuspendThread(Thread) : ResumeThread(Thread);
			if(Result != static_cast<DWORD>(-1))
				ChangedThread = true;
			CloseHandle(Thread);
		} while(Thread32Next(Snapshot, &Entry));
	}
	CloseHandle(Snapshot);
	return ChangedThread;
#elif defined(CONF_FAMILY_UNIX)
	return kill(process, paused ? SIGSTOP : SIGCONT) == 0;
#endif
}

void process_close(PROCESS process)
{
#if defined(CONF_FAMILY_WINDOWS)
	if(process != INVALID_PROCESS)
		CloseHandle(process);
#else
	(void)process;
#endif
}

int process_kill(PROCESS process)
{
#if defined(CONF_FAMILY_WINDOWS)
	BOOL success = TerminateProcess(process, 0);
	BOOL is_alive = process_is_alive(process);
	if(success || !is_alive)
	{
		CloseHandle(process);
		return true;
	}
	return false;
#elif defined(CONF_FAMILY_UNIX)
	if(!process_is_alive(process))
		return true;
	int status;
	kill(process, SIGTERM);
	return waitpid(process, &status, 0) != -1;
#endif
}

bool process_is_alive(PROCESS process)
{
	if(process == INVALID_PROCESS)
		return false;
#if defined(CONF_FAMILY_WINDOWS)
	DWORD exit_code;
	GetExitCodeProcess(process, &exit_code);
	return exit_code == STILL_ACTIVE;
#else
	return waitpid(process, nullptr, WNOHANG) == 0;
#endif
}

#endif // !defined(CONF_PLATFORM_ANDROID)
