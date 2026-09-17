// "Start with Windows".
//
// Two mechanisms, because the app runs fine un-elevated but SetSystemCursor
// does not:
//  * elevated  -> a Task Scheduler task with highest privileges, so no UAC
//    prompt per logon (what v1.3 attempted, minus the ATL dependency);
//  * not elevated -> the HKCU Run key, which needs no elevation at all.
#pragma once

#include "targetver.h"

namespace stfc {

class AutoStart {
 public:
  static constexpr const wchar_t* kTaskName = L"ShakeToFindCursor";
  static constexpr const wchar_t* kRunValueName = L"ShakeToFindCursor";

  static bool IsTaskRegistered();
  static bool IsRunKeySet();
  static bool IsEnabled();
  // Chooses the mechanism based on the current token.
  static bool Enable();
  static bool Disable();

 private:
  static bool EnableTask();
  static bool EnableRunKey();
  static bool DisableTask();
  static bool DisableRunKey();
};

}  // namespace stfc
