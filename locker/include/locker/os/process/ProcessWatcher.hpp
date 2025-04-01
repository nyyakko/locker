#pragma once

#include <liberror/Result.hpp>
#include <liberror/Try.hpp>

#include <combaseapi.h>
#include <comdef.h>
#include <cwchar>
#include <oleauto.h>
#include <processthreadsapi.h>
#include <string_view>
#include <wbemcli.h>
#include <WbemIdl.h>
#include <windows.h>
#include <TlHelp32.h>
#include <winerror.h>

liberror::Result<void> initialize_com();
liberror::Result<void> connect_to_wmi(IWbemLocator*& locator, IWbemServices*& service);
liberror::Result<void> set_wmi_proxy_blanket(IWbemLocator* locator, IWbemServices* service);
liberror::Result<IEnumWbemClassObject*> get_process_creation_event_listener(IWbemLocator* locator, IWbemServices* service);
liberror::Result<IEnumWbemClassObject*> get_process_deletion_event_listener(IWbemLocator* locator, IWbemServices* service);


