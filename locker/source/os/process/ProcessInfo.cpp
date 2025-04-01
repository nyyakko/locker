#include "os/process/ProcessInfo.hpp"

#include <processthreadsapi.h>
#include <psapi.h>

using namespace liberror;

ProcessInfo get_started_process_info(IWbemClassObject* object)
{
    ProcessInfo processInfo {};
    VARIANT variant;
    if (SUCCEEDED(object->Get(L"TargetInstance", 0, &variant, 0, 0)))
    {
        IUnknown* unknown = variant.punkVal;
        IWbemClassObject* process = nullptr;
        unknown->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(&process));
        if (process)
        {
            VARIANT processId;
            process->Get(L"ProcessId", 0, &processId, 0, 0);
            VARIANT processName;
            process->Get(L"Name", 0, &processName, 0, 0);

            std::wstring_view processNameView { processName.bstrVal };
            processInfo.name = std::string(processNameView.begin(), processNameView.end());
            processInfo.pid = static_cast<DWORD>(processId.intVal);

            VariantClear(&processName);
            VariantClear(&processId);
        }
    }
    VariantClear(&variant);
    return processInfo;
}

Result<DWORD> get_thread_id_from_pid(DWORD pid)
{
    auto snapshotHandler = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshotHandler == INVALID_HANDLE_VALUE) return make_error("CreateToolhelp32Snapshot failed.");

    THREADENTRY32 entry {};
    entry.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(snapshotHandler, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == pid)
            {
                CloseHandle(snapshotHandler);
                return entry.th32ThreadID;
            }
        } while (Thread32Next(snapshotHandler, &entry));
    }

    return -1;
}

Result<void> suspend_process_thread(ProcessInfo const& processInfo)
{
    auto processThreadId = MUST(get_thread_id_from_pid(processInfo.pid));
    auto processThreadHandle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, processThreadId);

    if (processThreadHandle == nullptr)
    {
        return make_error("OpenThread failed to suspend process");
    }

    SuspendThread(processThreadHandle);

    return {};
}

Result<void> resume_process_thread(ProcessInfo const& processInfo)
{
    auto processThreadId = MUST(get_thread_id_from_pid(processInfo.pid));
    auto processThreadHandle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, processThreadId);

    if (processThreadHandle == nullptr)
    {
        return make_error("OpenThread failed to suspend process");
    }

    ResumeThread(processThreadHandle);

    return {};
}

static std::string trim(std::string const& value)
{
    auto result = value;
    result.erase(result.begin(), std::ranges::find_if(result, [] (auto character) { return !std::isspace(character); }));
    result.erase(std::ranges::find_if(result.rbegin(), result.rend(), [] (auto character) { return !std::isspace(character); }).base(), result.end());
    return result;
}

Result<std::unordered_map<std::string, std::vector<ProcessInfo>>> get_running_processes()
{
    std::unordered_map<std::string, std::vector<ProcessInfo>> processes {};

    DWORD processesArray[1024];
    DWORD processCount;

    if (!EnumProcesses(processesArray, sizeof(processesArray), &processCount))
    {
        return make_error("Failed to fetch processes");
    }

    for (auto i = 0zu; i < processCount / sizeof(DWORD); i += 1)
    {
        auto pid = processesArray[i];
        if (pid == 0) continue;
        TCHAR processName[MAX_PATH] = TEXT("INVALID");
        HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (processHandle != nullptr)
        {
            HMODULE module;
            DWORD modulesCount;
            if (EnumProcessModules(processHandle, &module, 8, &modulesCount))
            {
                GetModuleBaseName(processHandle, module, processName, sizeof(processName)/sizeof(TCHAR));
            }
        }
        if (trim(processName) == "INVALID") continue;
        processes[trim(processName)].emplace_back(trim(processName), pid);
        CloseHandle(processHandle);
    }

    return processes;
}
