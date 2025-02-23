//-------------------------------------------------------------------------------------------------------
// Copyright (C) Tim Mangan. All rights reserved
// Copyright (C) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include <regex>
#include <psf_framework.h>
#include <psf_logging.h>
#include "FunctionImplementations.h"
#include "EnvVar_spec.h"


extern std::vector<env_var_spec> g_envvar_envVarSpecs;

DWORD g_EnvVarInterceptInstance = 0;


//ExpandEnvironmentStrings
//  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
//  public static extern int ExpandEnvironmentStrings([MarshalAs(UnmanagedType.LPTStr)] String source, [Out] StringBuilder destination, int size);


//GetEnvironmentVariable
//      DllImport("kernel32.dll")]
//  static extern uint GetEnvironmentVariable(string lpName,
//      [Out] StringBuilder lpBuffer, uint nSize);
//
auto GetEnvironmentVariableImpl = psf::detoured_string_function(&::GetEnvironmentVariableA, &::GetEnvironmentVariableW);
template <typename CharC, typename CharT>
DWORD __stdcall GetEnvironmentVariableFixup(_In_ const CharC* lpName, _Inout_ CharT* lpValue, _In_ DWORD lenBuf)
{
    DWORD GetEnvVarInstance = ++g_EnvVarInterceptInstance;
#if _DEBUG
    LogString(GetEnvVarInstance,L"GetEnvironmentVariableFixup called for", lpName);
#endif
    auto guard = g_reentrancyGuard.enter();
    DWORD result;

    if (guard)
    {
#if _DEBUG
        Log(L"[%d] GetEnvironmentVariableFixup unguarded.", GetEnvVarInstance);
#endif
        std::wstring eName;
        if constexpr (psf::is_ansi<CharT>)
        {
            eName = widen(lpName);
        }
        else
        {
            eName =lpName;
        }

        for (env_var_spec spec : g_envvar_envVarSpecs)
        {
            try
            {
                if (std::regex_match(eName, spec.variablename))
                {
                    size_t valuelen = spec.variablevalue.length();
                    if (spec.useregistry == true)
                    {
#if _DEBUG
                        Log(L"[%d] GetEnvironmentVariableFixup: Registry supplied case.", GetEnvVarInstance);
#endif
                        // Check app registry for an answer instead of the Json. Note: this allows value to be modified possibly also.
                        HKEY hKeyCU;
                        if constexpr (psf::is_ansi<CharT>)
                        {
                            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment",
                                0, KEY_ENUMERATE_SUB_KEYS | KEY_READ | KEY_QUERY_VALUE, &hKeyCU) == ERROR_SUCCESS)
                            {
#if _DEBUG
                                Log(L"[%d] GetEnvironmentVariableFixup:(A) HKCU key found.", GetEnvVarInstance);
#endif
                                DWORD type = RRF_RT_REG_SZ;
                                DWORD dLen = (DWORD)lenBuf;
                                auto ret = RegGetValueA(HKEY_CURRENT_USER, "Environment", lpName,
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_ZEROONFAILURE, &type, lpValue, &dLen);
                                if (ret == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(A) HKCU value found %s", GetEnvVarInstance, lpValue);
#endif
                                    result = dLen;
                                    RegCloseKey(hKeyCU);
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(A) HKCU value Failed 0x%x.", GetEnvVarInstance, ret);
#endif
                                }
                                RegCloseKey(hKeyCU);
                            }
                        }
                        else
                        {
                            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment",
                                0, KEY_ENUMERATE_SUB_KEYS | KEY_READ | KEY_QUERY_VALUE, &hKeyCU) == ERROR_SUCCESS)
                            {
#if _DEBUG
                                Log(L"[%d] GetEnvironmentVariableFixup:(W) HKCU key found.", GetEnvVarInstance);
                                LogString(GetEnvVarInstance, L"GetEnvironmentVariableFixup:(W) Looking for ", lpName);
#endif
                                DWORD type = RRF_RT_REG_SZ;
                                DWORD dLen = lenBuf;
                                auto ret = RegGetValueW(HKEY_CURRENT_USER, L"Environment", lpName,
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_ZEROONFAILURE, &type, lpValue, &dLen);
                                if (ret == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(W) HKCU value found %ls", GetEnvVarInstance, lpValue);
#endif
                                    result = dLen;
                                    RegCloseKey(hKeyCU);
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(W) HKCU value Failed 0x%x.", GetEnvVarInstance, ret);
#endif
                                }
                                RegCloseKey(hKeyCU);
                            }
                        }
                        result = 0;

                        HKEY hKeyLM;
                        if constexpr (psf::is_ansi<CharT>)
                        {
                            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                                0, KEY_ENUMERATE_SUB_KEYS | KEY_READ | KEY_QUERY_VALUE, &hKeyLM) == ERROR_SUCCESS)
                            {
#if _DEBUG
                                Log(L"[%d] GetEnvironmentVariableFixup:(A) HKLM key found.", GetEnvVarInstance);
#endif
                                LONG dLen0 = (LONG)lenBuf;
                                auto ret00 = RegQueryValueA(hKeyLM, lpName, lpValue, &dLen0);
                                if (ret00 == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(A) HKLM value queried! %s", GetEnvVarInstance, lpValue);
#endif
                                    result = (DWORD)dLen0;
                                    RegCloseKey(hKeyLM);
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(W) HKCU value Failed 0x%x.", GetEnvVarInstance, ret00);
#endif
                                }

                                RegCloseKey(hKeyLM);
                            }
                        }
                        else
                        {
                            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                                0, KEY_ENUMERATE_SUB_KEYS | STANDARD_RIGHTS_READ | KEY_QUERY_VALUE, &hKeyLM) == ERROR_SUCCESS)
                            {
#if _DEBUG
                                Log(L"[%d] GetEnvironmentVariableFixup:(W) HKLM key found.", GetEnvVarInstance);
#endif
                                DWORD type = RRF_RT_REG_SZ;
                                DWORD dLen = lenBuf;
                                auto ret = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", lpName,
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_ZEROONFAILURE, &type, lpValue, &dLen);
                                if (ret == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(W) HKLM value found %ls", GetEnvVarInstance, lpValue);
#endif
                                    result = dLen;
                                    RegCloseKey(hKeyLM);
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] GetEnvironmentVariableFixup:(W) HKLM value Failed 0x%x.", GetEnvVarInstance, ret);
#endif
                                }
                                RegCloseKey(hKeyLM);
                            }
                        }
                        // allow to fall through to system
                        //return result;
                        result = 0;
                    }

                    // Sometimes HKLM\System reg items from the package are not visible to the app.  We think this is a bug, so
                    // allow a check to see if there is a value field in the JSON and use that.
                    // Of course, we should always look at the json if useregistry was set to false anyway.

#if _DEBUG
                    Log(L"[%d] GetEnvironmentVariableFixup: Json supplied case.", GetEnvVarInstance);
#endif

                    if (valuelen < lenBuf)
                    {
#if _DEBUG
                        Log(L"[%d] GetEnvironmentVariableFixup: Match to be returned.", GetEnvVarInstance);
#endif
                        // copy into lpValue, but might need form conversion
                        if constexpr (psf::is_ansi<CharT>)
                        {

                            std::string sval = narrow(spec.variablevalue);
#if _DEBUG
                            LogString(GetEnvVarInstance, L"GetEnvironmentVariableFixup:(A) HKCU value is ", sval.c_str());
#endif
                            ZeroMemory(lpValue, lenBuf);
                            sval.copy(lpValue, lenBuf, 0);
                            //strcpy_s(lpValue, lenBuf, sval.c_str());
#if _DEBUG
                            LogString(GetEnvVarInstance, L"GetEnvironmentVariableFixup:(A) HKCU value copied is ", lpValue);
#endif
                            result = (DWORD)sval.length();
                        }
                        else
                        {
#if _DEBUG
                            LogString(GetEnvVarInstance, L"GetEnvironmentVariableFixup:(W) HKCU value is ", spec.variablevalue.data());
#endif
                            ZeroMemory(lpValue, lenBuf);
                            spec.variablevalue.copy(lpValue, lenBuf, 0);
#if _DEBUG
                            LogString(GetEnvVarInstance, L"GetEnvironmentVariableFixup:(W) HKCU value copied is ", lpValue);
#endif
                            result = (DWORD)valuelen;
                        }
#if _DEBUG
                        Log(L"GetEnvironmentVariableFixup: Value saved.");
#endif
                        SetLastError(ERROR_SUCCESS);
                        return(result);
                    }
                    else
                    {
#if _DEBUG
                        Log(L"GetEnvironmentVariableFixup: Match returns bufferoverflow.");
#endif
                        // return buffer overflow
                        result = ERROR_BUFFER_OVERFLOW;
                        SetLastError(ERROR_BUFFER_OVERFLOW);
                        return(result);
                    }

                }
            }
            catch (...)
            {
                Log(L"[%d] Bad Regex pattern ignored in EnvVarFixup.\n", GetEnvVarInstance);
            }
        }

        // If still here, make original call

#if _DEBUG
        Log(L"[%d] GetEnvironmentVariableFixup: No match - fall through.", GetEnvVarInstance);
#endif
    }
    result = GetEnvironmentVariableImpl(lpName, lpValue, lenBuf);
    return result;
}
DECLARE_STRING_FIXUP(GetEnvironmentVariableImpl, GetEnvironmentVariableFixup);


//SetEnvironmentVariable
//  [DllImport("kernel32.dll", SetLastError=true)]
//  static extern bool SetEnvironmentVariable(string lpName, string lpValue);
auto SetEnvironmentVariableImpl = psf::detoured_string_function(&::SetEnvironmentVariableA, &::SetEnvironmentVariableW);
template <typename CharT>
BOOL __stdcall SetEnvironmentVariableFixup(_In_ const CharT* lpName, _In_ const CharT* lpValue)
{
    DWORD SetEnvVarInstance = ++g_EnvVarInterceptInstance;
#if _DEBUG
    LogString(SetEnvVarInstance, L"SetEnvironmentVariableFixup called for", lpName);
#endif

    auto guard = g_reentrancyGuard.enter();
    BOOL result;

    if (guard)
    {
#if _DEBUG
        Log(L"[%d] SetEnvironmentVariableFixup unguarded.", SetEnvVarInstance);
#endif

        std::wstring eName;
        if constexpr (psf::is_ansi<CharT>)
        {
            eName = widen(lpName);
        }
        else
        {
            eName = lpName;
        }
        for (env_var_spec spec : g_envvar_envVarSpecs)
        {
            try
            {
                if (std::regex_match(eName, spec.variablename))
                {
                    if (spec.useregistry == true)
                    {
#if _DEBUG
                        Log(L"[%d] GetEnvironmentVariableFixup: registry case.", SetEnvVarInstance);
#endif
                        HKEY hKeyCU;
                        if constexpr (psf::is_ansi<CharT>)
                        {
                            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, MAXIMUM_ALLOWED, &hKeyCU) == ERROR_SUCCESS)
                            {
                                DWORD dLen = (DWORD)((strlen(lpValue) + 1) * sizeof(CharT));
                                auto ret = RegSetValueExA(hKeyCU, lpName, NULL, REG_SZ, (BYTE*)lpValue, dLen);
                                if (ret == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] SetEnvironmentVariableFixup: success. %s=%s", SetEnvVarInstance, lpName, lpValue);
#endif
                                    result = 1;
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] SetEnvironmentVariableFixup: Failure 0x%x.", SetEnvVarInstance, GetLastError());
#endif
                                    result = 0;
                                    return result;
                                }
                            }
                        }
                        else
                        {
                            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, MAXIMUM_ALLOWED, &hKeyCU) == ERROR_SUCCESS)
                            {
                                DWORD dLen = (DWORD)((wcslen(lpValue) + 1) * sizeof(CharT));
                                auto ret = RegSetValueExW(hKeyCU, lpName, NULL, REG_SZ, (BYTE*)lpValue, dLen);
                                if (ret == ERROR_SUCCESS)
                                {
#if _DEBUG
                                    Log(L"[%d] SetEnvironmentVariableFixup: success. %ls=%ls", SetEnvVarInstance, lpName, lpValue);
#endif
                                    result = 1;
                                    return result;
                                }
                                else
                                {
#if _DEBUG
                                    Log(L"[%d] SetEnvironmentVariableFixup: Failure 0x%x.", SetEnvVarInstance, GetLastError());
#endif
                                    result = 0;
                                    return result;
                                }
                            }
                        }
                    }
                    else
                    {
#if _DEBUG
                        Log(L"[%d] GetEnvironmentVariableFixup: JSON case - return ACCESS_DENIED.", SetEnvVarInstance);
#endif
                        // Unable to overwrite json, return ACCESS_DENIED
                        SetLastError(ERROR_ACCESS_DENIED);
                        result = 0;
                        return result;
                    }
                }
            }
            catch (...)
            {
                Log(L"[%d] Bad Regex pattern ignored in EnvVarFixup.\n", SetEnvVarInstance);
            }
        }
    }

    // If still here, make original call

#if _DEBUG
    Log(L"[%d] SetEnvironmentVariableFixup: No match - fall through.", SetEnvVarInstance);
#endif
    result = SetEnvironmentVariableImpl(lpName, lpValue);
    return result;
}
DECLARE_STRING_FIXUP(SetEnvironmentVariableImpl, SetEnvironmentVariableFixup);
