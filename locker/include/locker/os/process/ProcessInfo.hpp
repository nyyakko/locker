#pragma once

#include <liberror/Result.hpp>
#include <liberror/Try.hpp>

#include <combaseapi.h>
#include <comdef.h>
#include <cwchar>
#include <oleauto.h>
#include <processthreadsapi.h>
#include <wbemcli.h>
#include <WbemIdl.h>
#include <windows.h>
#include <TlHelp32.h>
#include <winerror.h>

#include <unordered_map>
#include <string_view>
#include <vector>

struct ProcessInfo
{
    std::string name;
    DWORD pid;

    bool operator==(ProcessInfo const& that) const
    {
        return this->name == that.name;
    }
};

ProcessInfo get_started_process_info(IWbemClassObject* object);
liberror::Result<DWORD> get_thread_id_from_pid(DWORD pid);
liberror::Result<void> suspend_process_thread(ProcessInfo const& processInfo);
liberror::Result<void> resume_process_thread(ProcessInfo const& processInfo);
liberror::Result<std::unordered_map<std::string, std::vector<ProcessInfo>>> get_running_processes();
