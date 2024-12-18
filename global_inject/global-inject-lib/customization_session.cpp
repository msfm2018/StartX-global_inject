
#include "stdafx.h"
#include "customization_session.h"
#include "session_private_namespace.h"
#include "logger.h"
#include <initguid.h>  // must come before knownfolders.h

#include <inspectable.h>
#include <knownfolders.h>
#include <psapi.h>

#include <wininet.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Version.lib")

extern HINSTANCE g_hDllInst;

DWORD g_refreshIconThreadId;
bool g_refreshIconNeedToAdjustTimer;
bool g_inGetTimeToolTipString;
const WCHAR* myStr = L"中国近现代史纲要必过！";
enum class WinVersion {
	Unsupported,
	Win10,
	Win11,
	Win11_22H2,
	Win11_24H2,
};

WinVersion g_winVersion;


typedef LONG(WINAPI* RtlGetVersionFunc)(OSVERSIONINFOEXW*);


WinVersion GetOSVersion1() {
	OSVERSIONINFOEXW osvi = { sizeof(OSVERSIONINFOEXW) };
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

	if (hNtdll) {
		auto RtlGetVersion = (RtlGetVersionFunc)GetProcAddress(hNtdll, "RtlGetVersion");
		if (RtlGetVersion && RtlGetVersion(&osvi) == 0) {
			/*std::wcout << L"Operating System Version: "
				<< osvi.dwMajorVersion << L"."
				<< osvi.dwMinorVersion << L"."
				<< osvi.dwBuildNumber << std::endl;*/
			LOG(L"Version: %u.%u.%u", osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);

			switch (osvi.dwMajorVersion) {
			case 10:
				if (osvi.dwBuildNumber < 22000) {
					return WinVersion::Win10;
				}
				else if (osvi.dwBuildNumber <= 22000) {
					return WinVersion::Win11;
				}
				else if (osvi.dwBuildNumber < 26100) {
					return WinVersion::Win11_22H2;
				}
				else {
					return WinVersion::Win11_24H2;
				}
				break;
			}

		
			
		}
	}
	return WinVersion::Unsupported;

}
// 触发任务栏相关更新的函数
void ApplySettingsWin11() {
	DWORD dwProcessId;
	DWORD dwCurrentProcessId = GetCurrentProcessId();
	HWND hTaskbarWnd = FindWindow(L"Shell_TrayWnd", nullptr);

	if (hTaskbarWnd && GetWindowThreadProcessId(hTaskbarWnd, &dwProcessId) && dwProcessId == dwCurrentProcessId) {
		constexpr WCHAR kTempValueName[] = L"_clock-customization";
		HKEY hSubKey;

		LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Control Panel\\TimeDate\\AdditionalClocks", 0, KEY_WRITE, &hSubKey);
		if (result == ERROR_SUCCESS) {
			if (RegSetValueEx(hSubKey, kTempValueName, 0, REG_SZ, (const BYTE*)L"", sizeof(WCHAR)) == ERROR_SUCCESS) {
				if (RegDeleteValue(hSubKey, kTempValueName) != ERROR_SUCCESS) {
					LOG(L"Failed to remove temp value");
				}
			}
			else {
				LOG(L"Failed to create temp value");
			}
			RegCloseKey(hSubKey);
		}
		else {
			LOG(L"Failed to open subkey: %d", result);
		}
	}
}


namespace
{
	// 获取基于系统区域设置的时间段（上午/下午 或 AM/PM）
	const wchar_t* GetLocalizedTimePeriod(const SYSTEMTIME* time) {
		static wchar_t timePeriod[16]; // 保存返回的时间段
		wchar_t localeName[LOCALE_NAME_MAX_LENGTH];

		// 获取当前系统的区域设置名称（如 zh-CN 或 en-US）
		if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) == 0) {
			return (time->wHour < 12) ? L"AM" : L"PM"; // 默认英文
		}

		// 根据时间段和区域设置获取合适的本地化字符串
		int result = GetLocaleInfoEx(
			localeName,
			(time->wHour < 12) ? LOCALE_S1159 : LOCALE_S2359, // LOCALE_S1159 = 上午，LOCALE_S2359 = 下午
			timePeriod,
			sizeof(timePeriod) / sizeof(timePeriod[0])
		);

		if (result > 0) {
			return timePeriod; // 返回本地化时间段
		}
		else {
			return (time->wHour < 12) ? L"AM" : L"PM"; // 备用英文
		}
	}



	using GetLocalTime_t = decltype(&GetLocalTime);
	GetLocalTime_t GetLocalTime_Original;

	using GetTimeFormatEx_t = decltype(&GetTimeFormatEx);
	GetTimeFormatEx_t GetTimeFormatEx_Original;

	using GetDateFormatEx_t = decltype(&GetDateFormatEx);
	GetDateFormatEx_t GetDateFormatEx_Original;

	
	// 获取基于系统区域设置的星期几
	void GetLocalizedDayOfWeek(const SYSTEMTIME& time, wchar_t* dayOfWeek, size_t size) {
		wchar_t localeName[LOCALE_NAME_MAX_LENGTH];
		// 获取当前系统的区域设置名称（如 zh-CN 或 en-US）
		if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) != 0) {
			// `LOCALE_SDAYNAME1` 表示星期一，`wDayOfWeek` 的范围是 0（星期日）到 6（星期六）
			int offset = (time.wDayOfWeek == 0) ? 6 : (time.wDayOfWeek - 1); // 调整偏移
			GetLocaleInfoEx(
				localeName,
				LOCALE_SDAYNAME1 + offset, // 计算正确的偏移
				dayOfWeek,
				static_cast<int>(size)
			);
		}
		else {
			// 默认英文星期
			const wchar_t* defaultDaysOfWeek[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
			wcscpy_s(dayOfWeek, size, defaultDaysOfWeek[time.wDayOfWeek]);
		}
	}

	int WINAPI GetTimeFormatEx_Hook_Win11(LPCWSTR lpLocaleName, DWORD dwFlags, CONST SYSTEMTIME* lpTime, LPCWSTR lpFormat, LPWSTR lpTimeStr, int cchTime) {
		SYSTEMTIME currentTime;

		// 使用提供的时间或获取本地时间
		if (lpTime) {
			currentTime = *lpTime;
		}
		else {
			GetLocalTime(&currentTime);
		}

		// 获取本地化的星期几
		wchar_t dayOfWeek[16];
		GetLocalizedDayOfWeek(currentTime, dayOfWeek, sizeof(dayOfWeek) / sizeof(dayOfWeek[0]));

		// 格式化日期和时间
		int written = swprintf_s(lpTimeStr, cchTime,
			L"%s, %02d月%02d %d | %02d:%02d:%02d %s",
			dayOfWeek,
			currentTime.wMonth,
			currentTime.wDay,
			currentTime.wYear,
			currentTime.wHour,
			currentTime.wMinute,
			currentTime.wSecond,
			GetLocalizedTimePeriod(&currentTime)); // 根据区域动态获取时间段

		// 检查格式化是否成功
		if (written < 0 || static_cast<size_t>(written) >= cchTime) {
			return 0; // 格式化失败
		}

		g_refreshIconNeedToAdjustTimer = true;
		return written;
	}




	int WINAPI GetDateFormatEx_Hook_Win11(LPCWSTR lpLocaleName, DWORD dwFlags, CONST SYSTEMTIME* lpDate, LPCWSTR lpFormat, LPWSTR lpDateStr, int cchDate, LPCWSTR lpCalendar) 
	{
		
		wcscpy(lpDateStr, myStr);
		return (int)wcslen(myStr);
	}

	VOID WINAPI GetLocalTime_Hook_Win11(LPSYSTEMTIME lpSystemTime) 
	{
		DWORD dwProcessId;
		DWORD dwCurrentProcessId = GetCurrentProcessId();
		HWND hTaskbarWnd = FindWindow(L"Shell_TrayWnd", nullptr);

		if (hTaskbarWnd && GetWindowThreadProcessId(hTaskbarWnd, &dwProcessId) && dwProcessId == dwCurrentProcessId && g_refreshIconNeedToAdjustTimer) {
			g_refreshIconNeedToAdjustTimer = false;

			// Make the next refresh happen in a second.
			memset(lpSystemTime, 0, sizeof(*lpSystemTime));
			lpSystemTime->wSecond = 59;
			return;
		}

		GetLocalTime_Original(lpSystemTime);
	}


	MH_STATUS InitCustomizationHooks()
	{

		HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
		if (!kernelBaseModule) {
			LOG(L"Failed to get kernelbase.dll handle");
			return MH_ERROR_MODULE_NOT_FOUND;
		}
	
			MH_STATUS status;
			auto pGetLocalTime = GetProcAddress(kernelBaseModule, "GetLocalTime");
			if (!pGetLocalTime) {
				return MH_UNKNOWN;
			}
				status = MH_CreateHook((void*)pGetLocalTime, (void*)GetLocalTime_Hook_Win11, (void**)&GetLocalTime_Original);

			if (status == MH_OK) {
				status = MH_QueueEnableHook(pGetLocalTime);
			}
			else {
				return status;
			}

			auto pGetTimeFormatEx = GetProcAddress(kernelBaseModule, "GetTimeFormatEx");
			if (!pGetTimeFormatEx) {
				return MH_UNKNOWN;
			}
			status = MH_CreateHook((void*)pGetTimeFormatEx, (void*)GetTimeFormatEx_Hook_Win11, (void**)&GetTimeFormatEx_Original);
			if (status == MH_OK) {
				status = MH_QueueEnableHook(pGetTimeFormatEx);
			}
			else {
				return status;
			}

			auto pGetDateFormatEx = GetProcAddress(kernelBaseModule, "GetDateFormatEx");
			if (!pGetDateFormatEx) {
				return MH_UNKNOWN;
			}
			status = MH_CreateHook(pGetDateFormatEx, (void*)GetDateFormatEx_Hook_Win11, (void**)&GetDateFormatEx_Original);
			if (status == MH_OK) {
				status = MH_QueueEnableHook(pGetDateFormatEx);
			}
			return status;
	}
}

bool CustomizationSession::Start(bool runningFromAPC, HANDLE sessionManagerProcess, HANDLE sessionMutex) noexcept
{
	auto instance = new (std::nothrow) CustomizationSession();
	if (!instance) {
		LOG(L"Allocation of CustomizationSession failed");
		return false;
	}

	if (!instance->StartAllocated(runningFromAPC, sessionManagerProcess, sessionMutex)) {
		delete instance;
		return false;
	}

	// Instance will free itself.
	return true;
}

bool CustomizationSession::StartAllocated(bool runningFromAPC, HANDLE sessionManagerProcess, HANDLE sessionMutex) noexcept
{
	// Create the session semaphore. This will block the library if another instance
	// (from another session manager process) is already injected and its customization session is active.
	WCHAR szSemaphoreName[sizeof("CustomizationSessionSemaphore-pid=1234567890")];
	swprintf_s(szSemaphoreName, L"CustomizationSessionSemaphore-pid=%u", GetCurrentProcessId());

	HRESULT hr = m_sessionSemaphore.create(1, 1, szSemaphoreName);
	if (FAILED(hr)) {
		LOG(L"Semaphore creation failed with error %08X", hr);
		return false;
	}

	m_sessionSemaphoreLock = m_sessionSemaphore.acquire();

	if (WaitForSingleObject(sessionManagerProcess, 0) != WAIT_TIMEOUT) {
		VERBOSE(L"Session manager process is no longer running");
		return false;
	}

	if (!InitSession(runningFromAPC, sessionManagerProcess)) {
		return false;
	}

	if (runningFromAPC) {
		// Create a new thread for us to allow the program's main thread to run.
		try {
			// Note: Before creating the thread, the CRT/STL bumps the
			// reference count of the module, something a plain CreateThread
			// doesn't do.
			std::thread thread(&CustomizationSession::RunAndDeleteThis, this,
				sessionManagerProcess, sessionMutex);
			thread.detach();
		}
		catch (const std::exception& e) {
			LOG(L"%S", e.what());
			UninitSession();
			return false;
		}
	}
	else {
		// No need to create a new thread, a dedicated thread was created for us
		// before injection.
		RunAndDeleteThis(sessionManagerProcess, sessionMutex);
	}

	return true;
}


bool CustomizationSession::InitSession(bool runningFromAPC, HANDLE sessionManagerProcess) noexcept
{
	g_refreshIconThreadId = GetCurrentThreadId();
	g_refreshIconNeedToAdjustTimer = true;

	MH_STATUS status = MH_Initialize();
	if (status != MH_OK) {
		LOG(L"MH_Initialize failed with %d", status);
		return false;
	}

	if (runningFromAPC) {
		// No other threads should be running, skip thread freeze.
		MH_SetThreadFreezeMethod(MH_FREEZE_METHOD_NONE_UNSAFE);
	}
	else {
		MH_SetThreadFreezeMethod(MH_FREEZE_METHOD_FAST_UNDOCUMENTED);
	}

	try {
		m_newProcessInjector.emplace(sessionManagerProcess);
	}
	catch (const std::exception& e) {
		LOG(L"InitSession failed: %S", e.what());
		m_newProcessInjector.reset();
		MH_Uninitialize();
		return false;
	}

	status = InitCustomizationHooks();
	if (status != MH_OK) {
		LOG(L"InitCustomizationHooks failed with %d", status);
	}

	status = MH_ApplyQueued();


	if (status != MH_OK) {
		LOG(L"MH_ApplyQueued failed with %d", status);
	}

	if (runningFromAPC) {
		MH_SetThreadFreezeMethod(MH_FREEZE_METHOD_FAST_UNDOCUMENTED);
	}

	g_winVersion=GetOSVersion1();
	if (g_winVersion >= WinVersion::Win11) {
		ApplySettingsWin11();
	}


	return true;
}

void CustomizationSession::RunAndDeleteThis(HANDLE sessionManagerProcess, HANDLE sessionMutex) noexcept
{
	m_sessionManagerProcess.reset(sessionManagerProcess);

	if (sessionMutex) {
		m_sessionMutex.reset(sessionMutex);
	}

	// Prevent the system from displaying the critical-error-handler message box.
	// A message box like this was appearing while trying to load a dll in a
	// process with the ProcessSignaturePolicy mitigation, and it looked like this:
	// https://stackoverflow.com/q/38367847
	DWORD dwOldMode;
	SetThreadErrorMode(SEM_FAILCRITICALERRORS, &dwOldMode);

	Run();

	SetThreadErrorMode(dwOldMode, nullptr);

	delete this;
}

void CustomizationSession::Run() noexcept
{
	DWORD waitResult = WaitForSingleObject(m_sessionManagerProcess.get(), INFINITE);
	if (waitResult != WAIT_OBJECT_0) {
		LOG(L"WaitForSingleObject returned %u, last error %u", waitResult, GetLastError());
	}

	VERBOSE(L"Uninitializing and freeing library");

	UninitSession();
}

void CustomizationSession::UninitSession() noexcept
{
	MH_STATUS status = MH_Uninitialize();
	if (status != MH_OK) {
		LOG(L"MH_Uninitialize failed with status %d", status);
	}
	
	if (g_winVersion >= WinVersion::Win11) {
		ApplySettingsWin11();
	}
	m_newProcessInjector.reset();
}

