#include "targetver.h"

#include "auto_start.h"

#include <taskschd.h>

#include "app_settings.h"
#include "com_ptr.h"
#include "win_util.h"

// Only the interfaces' vtables are used, and the CLSID/IID values below are
// spelled by hand, so nothing has to be imported from taskschd.dll: this file
// needs no taskschd.lib / uuid.lib, which also keeps it linkable with a
// toolchain that has no import library for that DLL. CoCreateInstance and the
// BSTR helpers do come from ole32.
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#endif

namespace stfc {
namespace {

// Hard-coded so the build needs neither uuid.lib nor initguid.h.
const GUID kCLSIDTaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc,
    {0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd}};
const GUID kIIDTaskService = {
    0x2faba4c7, 0x4da9, 0x4013,
    {0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85}};
const GUID kIIDLogonTrigger = {
    0x72dade38, 0xfae4, 0x4b3e,
    {0xba, 0xf4, 0x5d, 0x00, 0x9a, 0xf0, 0x2b, 0x1c}};
const GUID kIIDExecAction = {
    0x4c3d624d, 0xfd6b, 0x49a3,
    {0xb9, 0xb7, 0x09, 0xcb, 0x3c, 0xd3, 0xf0, 0x47}};

// COM is only needed for this file, and only for as long as a call lasts, so
// the message-loop thread never has to commit to an apartment model.
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    owns_ = SUCCEEDED(hr_);
  }
  ~ComScope() {
    if (owns_) CoUninitialize();
  }
  bool ok() const { return owns_ || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_;
  bool owns_ = false;
};

class BStr {
 public:
  explicit BStr(const wchar_t* text) : value_(SysAllocString(text)) {}
  ~BStr() {
    if (value_) SysFreeString(value_);
  }
  BSTR get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  BStr(const BStr&) = delete;
  BStr& operator=(const BStr&) = delete;
  BSTR value_;
};

VARIANT EmptyVariant() {
  VARIANT value;
  VariantInit(&value);
  return value;
}

bool ConnectTaskService(ComPtr<ITaskService>* service,
                        ComPtr<ITaskFolder>* root_folder) {
  IUnknown* unknown = nullptr;
  const HRESULT hr = CoCreateInstance(kCLSIDTaskScheduler, nullptr,
                                      CLSCTX_INPROC_SERVER, kIIDTaskService,
                                      reinterpret_cast<void**>(&unknown));
  if (FAILED(hr)) {
    return false;
  }
  ComPtr<ITaskService> connected;
  connected.Attach(static_cast<ITaskService*>(unknown));
  if (FAILED(connected->Connect(EmptyVariant(), EmptyVariant(), EmptyVariant(),
                                EmptyVariant()))) {
    return false;
  }
  ComPtr<ITaskFolder> root;
  if (FAILED(connected->GetFolder(BStr(L"\\").get(), root.Receive()))) {
    return false;
  }
  service->Attach(connected.Get());
  connected.Reset();  // service now owned by the caller
  root_folder->Attach(root.Get());
  root.Reset();
  return true;
}

}  // namespace

bool AutoStart::IsTaskRegistered() {
  ComScope com;
  if (!com.ok()) return false;
  ComPtr<ITaskService> service;
  ComPtr<ITaskFolder> root;
  if (!ConnectTaskService(&service, &root)) return false;
  ComPtr<IRegisteredTask> task;
  const HRESULT hr = root->GetTask(BStr(kTaskName).get(), task.Receive());
  return SUCCEEDED(hr) && task;
}

bool AutoStart::IsRunKeySet() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool set =
      RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr, nullptr) ==
      ERROR_SUCCESS;
  RegCloseKey(key);
  return set;
}

bool AutoStart::IsEnabled() { return IsTaskRegistered() || IsRunKeySet(); }

bool AutoStart::Enable() {
  if (IsElevated()) {
    if (EnableTask()) return true;
    LogCritical(
        "scheduled task could not be registered; falling back to the Run key");
  }
  return EnableRunKey();
}

bool AutoStart::Disable() {
  const bool task = DisableTask();
  const bool run = DisableRunKey();
  // Disabling a mechanism that was never enabled is not a failure; the user
  // just wants the app out of their start-up.
  return task || run || (!IsTaskRegistered() && !IsRunKeySet());
}

bool AutoStart::EnableTask() {
  wchar_t exe[MAX_PATH];
  GetExePath(exe, MAX_PATH);
  if (exe[0] == L'\0') return false;
  wchar_t working[MAX_PATH];
  StringCchCopyW(working, MAX_PATH, exe);
  RemoveFileName(working);

  ComScope com;
  if (!com.ok()) {
    LogCritical("COM unavailable, cannot register the start-up task");
    return false;
  }
  ComPtr<ITaskService> service;
  ComPtr<ITaskFolder> root;
  if (!ConnectTaskService(&service, &root)) {
    LogCritical("Task Scheduler unavailable");
    return false;
  }

  // A stale task (for example one pointing at a deleted exe) is replaced.
  root->DeleteTask(BStr(kTaskName).get(), 0);

  ComPtr<ITaskDefinition> definition;
  if (FAILED(service->NewTask(0, definition.Receive()))) return false;

  ComPtr<IRegistrationInfo> info;
  if (SUCCEEDED(definition->get_RegistrationInfo(info.Receive())) && info) {
    info->put_Description(BStr(L"Shake to Find Cursor: enlarge the pointer when "
                               L"the mouse is shaken.")
                              .get());
    info->put_Author(BStr(L"ShakeToFindCursor").get());
  }

  ComPtr<IPrincipal> principal;
  if (SUCCEEDED(definition->get_Principal(principal.Receive())) && principal) {
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
  }

  ComPtr<ITaskSettings> settings;
  if (SUCCEEDED(definition->get_Settings(settings.Receive())) && settings) {
    settings->put_StartWhenAvailable(VARIANT_TRUE);
    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    settings->put_Hidden(VARIANT_FALSE);
    BStr no_limit(L"PT0S");
    settings->put_ExecutionTimeLimit(no_limit.get());
  }

  ComPtr<ITriggerCollection> triggers;
  if (SUCCEEDED(definition->get_Triggers(triggers.Receive())) && triggers) {
    ComPtr<ITrigger> trigger;
    if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, trigger.Receive())) &&
        trigger) {
      BStr id(L"ShakeToFindCursorLogon");
      trigger->put_Id(id.get());
      trigger->put_Enabled(VARIANT_TRUE);
      // Give Explorer a moment, so the very first tray icon add succeeds.
      ComPtr<ILogonTrigger> logon;
      if (logon.From(trigger.Get(), kIIDLogonTrigger)) {
        BStr delay(L"PT5S");
        logon->put_Delay(delay.get());
      }
    }
  }

  ComPtr<IActionCollection> actions;
  if (SUCCEEDED(definition->get_Actions(actions.Receive())) && actions) {
    ComPtr<IAction> action;
    const HRESULT create_hr = actions->Create(TASK_ACTION_EXEC, action.Receive());
  if (SUCCEEDED(create_hr) && action) {
      ComPtr<IExecAction> exec;
      if (exec.From(action.Get(), kIIDExecAction)) {
        BStr path(exe);
        BStr directory(working);
        exec->put_Path(path.get());
        exec->put_WorkingDirectory(directory.get());
      }
    }
  }

  ComPtr<IRegisteredTask> registered;
  const HRESULT hr = root->RegisterTaskDefinition(
      BStr(kTaskName).get(), definition.Get(), TASK_CREATE_OR_UPDATE,
      EmptyVariant(), EmptyVariant(), TASK_LOGON_INTERACTIVE_TOKEN,
      EmptyVariant(), registered.Receive());
  if (FAILED(hr)) {
    LogCritical("RegisterTaskDefinition failed (0x%08lX)",
                static_cast<unsigned long>(hr));
    return false;
  }
  Logf("start-up task registered");
  return true;
}

bool AutoStart::EnableRunKey() {
  wchar_t exe[MAX_PATH];
  GetExePath(exe, MAX_PATH);
  if (exe[0] == L'\0') return false;
  wchar_t command[MAX_PATH + 8];
  StringCchCopyW(command, MAX_PATH + 8, L"\"");
  StringCchCatW(command, MAX_PATH + 8, exe);
  StringCchCatW(command, MAX_PATH + 8, L"\"");

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  const LONG result = RegSetValueExW(
      key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command),
      static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool AutoStart::DisableTask() {
  ComScope com;
  if (!com.ok()) return false;
  ComPtr<ITaskService> service;
  ComPtr<ITaskFolder> root;
  if (!ConnectTaskService(&service, &root)) return false;
  return SUCCEEDED(root->DeleteTask(BStr(kTaskName).get(), 0));
}

bool AutoStart::DisableRunKey() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  const LONG result = RegDeleteValueW(key, kRunValueName);
  RegCloseKey(key);
  return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

}  // namespace stfc
