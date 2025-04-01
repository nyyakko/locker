#include "os/process/ProcessWatcher.hpp"

using namespace liberror;

Result<void> initialize_com()
{
    auto result = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(result))
    {
        return make_error("Failed to initialize COM!");
    }

    result = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(result))
    {
        CoUninitialize();
        return make_error("CoInitializeSecurity failed with: {:x}", result);
    }

    return {};
}

Result<void> connect_to_wmi(IWbemLocator*& locator, IWbemServices*& service)
{
    auto result = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<LPVOID*>(&locator));
    if (FAILED(result))
    {
        CoUninitialize();
        return make_error("CoCreateInstance failed with: {:x}", result);
    }

    result = locator->ConnectServer(BSTR(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &service);
    if (FAILED(result))
    {
        CoUninitialize();
        return make_error("ConnectServer failed with: {:x}", result);
    }

    return {};
}

Result<void> set_wmi_proxy_blanket(IWbemLocator* locator, IWbemServices* service)
{
    auto result = CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    if (FAILED(result))
    {
        locator->Release();
        CoUninitialize();
        return make_error("CoSetProxyBlanket failed with: {:x}", result);
    }

    return {};
}

Result<IEnumWbemClassObject*> get_process_creation_event_listener(IWbemLocator* locator, IWbemServices* service)
{
    auto constexpr static query = BSTR(L"SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");

    IEnumWbemClassObject* enumerator = nullptr;
    auto result = service->ExecNotificationQuery(BSTR(L"WQL"), query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &enumerator);

    if (FAILED(result))
    {
        locator->Release();
        CoUninitialize();
        return make_error("ExecNotificationQuery failed with: {:x}", result);
    }

    return enumerator;
}

Result<IEnumWbemClassObject*> get_process_deletion_event_listener(IWbemLocator* locator, IWbemServices* service)
{
    auto constexpr static query = BSTR(L"SELECT * FROM __InstanceDeletionEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");

    IEnumWbemClassObject* enumerator = nullptr;
    auto result = service->ExecNotificationQuery(BSTR(L"WQL"), query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &enumerator);

    if (FAILED(result))
    {
        locator->Release();
        CoUninitialize();
        return make_error("ExecNotificationQuery failed with: {:x}", result);
    }

    return enumerator;
}
