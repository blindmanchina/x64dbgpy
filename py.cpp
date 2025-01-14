#include "py.h"
#include "stringutils.h"
#include "resource.h"
#include <windows.h>
#include <stdio.h>
#include <psapi.h>
#include <python.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

#define module_name "x64dbgpy"
#define token_paste(a, b) token_paste_(a, b)
#define token_paste_(a, b) a ## b
#define event_object_name "Event"
#define autorun_directory L"plugins\\x64dbgpy\\x64dbgpy\\autorun"
// lParam: ScanCode=0x41(ALT), cRepeat=1, fExtended=False, fAltDown=True, fRepeat=False, fUp=False
#define ALT_F7_SYSKEYDOWN 0x20410001

PyObject* pModule, *pEventObject;
HINSTANCE hInst;
HMODULE hPython27;

// This is necessary because python27 is delay loaded
#define PYDATA(symbol) []() -> decltype(symbol)& { static auto ptr = GetProcAddress(hPython27, #symbol); return *(decltype(&symbol))ptr; }()

enum
{
    MENU_RUNSCRIPTASYNC,
    MENU_RUNGUISCRIPT,
    MENU_ABOUT
};

extern "C" __declspec(dllexport) void CBMENUENTRY(CBTYPE cbType, PLUG_CB_MENUENTRY* info)
{
    switch(info->hEntry)
    {
    case MENU_RUNSCRIPTASYNC:
        DbgCmdExec("PyRunScriptAsync");
        break;

    case MENU_RUNGUISCRIPT:
        DbgCmdExec("PyRunGuiScript");
        break;

    case MENU_ABOUT:
        MessageBoxA(hwndDlg, "Made By RealGame (Tomer Zait)", plugin_name " Plugin", MB_ICONINFORMATION);
        break;
    }
}

static void pyCallback(const char* eventName, PyObject* pKwargs)
{
    PyObject* pFunc, *pValue;

    // Check if event object exist.
    if(pEventObject == NULL)
        return;

    pFunc = PyObject_GetAttrString(pEventObject, eventName);
    if(pFunc && PyCallable_Check(pFunc))
    {
        pValue = PyObject_Call(pFunc, PyTuple_New(0), pKwargs);
        Py_DECREF(pKwargs);
        Py_DECREF(pFunc);
        if(pValue == NULL)
        {
            _plugin_logprintf("[PYTHON] Could not use %s function.\n", eventName);
            PyErr_PrintEx(0);
            return;
        }

        Py_DECREF(pValue);
    }
}

static bool OpenFileDialog(wchar_t Buffer[MAX_PATH])
{
    OPENFILENAMEW sOpenFileName = { 0 };
    const wchar_t szFilterString[] = L"Python files\0*.py\0\0";
    const wchar_t szDialogTitle[] = L"Select script file...";
    sOpenFileName.lStructSize = sizeof(sOpenFileName);
    sOpenFileName.lpstrFilter = szFilterString;
    sOpenFileName.nFilterIndex = 1;
    sOpenFileName.lpstrFile = Buffer;
    sOpenFileName.nMaxFile = MAX_PATH;
    sOpenFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    sOpenFileName.lpstrTitle = szDialogTitle;
    sOpenFileName.hwndOwner = GuiGetWindowHandle();
    return (FALSE != GetOpenFileNameW(&sOpenFileName));
}

static bool FileExists(const wchar_t* file)
{
    DWORD attrib = GetFileAttributesW(file);
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

static bool ExecutePythonScript(const wchar_t* szFileName, int argc, char* argv[])
{
    if(!FileExists(szFileName))
    {
        _plugin_logputs("[PYTHON] File does not exist...");
        return false;
    }

    std::vector<wchar_t> szShortFileName;
    szShortFileName.resize(wcslen(szFileName) * 2);
    GetShortPathNameW(szFileName, szShortFileName.data(), DWORD(szShortFileName.size()));
    String szFileNameA = Utf16ToUtf8(szShortFileName.data());
    PyObject* PyFileObject = PyFile_FromString((char*)szFileNameA.c_str(), "r");
    if(PyFileObject == NULL)
    {
        _plugin_logputs("[PYTHON] Could not open file....");
        PyErr_PrintEx(0);
        return false;
    }

    bool local = false;

    PyObject* module, *dict;
    module = PyImport_AddModule("__main__");
    dict = PyModule_GetDict(module);
    if(local)
    {
        dict = PyDict_Copy(dict);
    }
    else
    {
        Py_INCREF(dict); // avoid to further distinguish between local and global dict
    }

    if(PyDict_GetItemString(dict, "__file__") == NULL)
    {
        PyObject* f = PyString_FromString(szFileNameA.c_str());
        if(f == NULL)
        {
            Py_DECREF(dict);
            return false;
        }
        if(PyDict_SetItemString(dict, "__file__", f) < 0)
        {
            Py_DECREF(f);
            Py_DECREF(dict);
            return false;
        }
        Py_DECREF(f);
    }

    wchar_t szCurrentDir[MAX_PATH] = L"";
    GetCurrentDirectoryW(_countof(szCurrentDir), szCurrentDir);

    if(argc > 0)
        PySys_SetArgv(argc, argv);
    auto result = PyRun_File(PyFile_AsFile(PyFileObject), szFileNameA.c_str(), Py_file_input, dict, dict);
    SetCurrentDirectoryW(szCurrentDir);
    Py_DECREF(dict);
    Py_DECREF(PyFileObject);

    if(result == NULL)
    {
        if(PyErr_ExceptionMatches(PYDATA(PyExc_SystemExit)))
            _plugin_logprintf("[PYTHON] SystemExit...\n");
        else
            _plugin_logprintf("[PYTHON] Exception...\n");
        PyErr_PrintEx(1);
        return false;
    }
    else
        Py_DECREF(result);

    _plugin_logputs("[PYTHON] Execution is done!");
    GuiUpdateAllViews();
    return true;
}

// Exports for other plugins
extern "C" __declspec(dllexport) bool ExecutePythonScriptExA(const char* szFileName, int argc, char* argv[])
{
    _plugin_logprintf("[PYTHON] Executing script: \"%s\"\n", szFileName);
    return ExecutePythonScript(Utf8ToUtf16(szFileName).c_str(), argc, argv);
}

extern "C" __declspec(dllexport) bool ExecutePythonScriptA(const char* szFileName)
{
    return ExecutePythonScriptExA(szFileName, 0, nullptr);
}

extern "C" __declspec(dllexport) bool ExecutePythonScriptExW(const wchar_t* szFileName, int argc, wchar_t* argv[])
{
    std::vector<char*> argvPtr(argc);
    std::vector<std::vector<char>> argvData(argc);
    for(int i = 0; i < argc; i++)
    {
        auto conv = Utf16ToUtf8(argv[i]);
        argvData[i] = std::vector<char>(conv.begin(), conv.end());
        argvData[i].push_back('\0');
        argvPtr[i] = argvData[i].data();
    }
    _plugin_logprintf("[PYTHON] Executing script: \"%s\"\n", Utf16ToUtf8(szFileName).c_str());
    return ExecutePythonScript(szFileName, argc, argvPtr.data());
}

extern "C" __declspec(dllexport) bool ExecutePythonScriptW(const wchar_t* szFileName)
{
    return ExecutePythonScriptExW(szFileName, 0, nullptr);
}

// Command callbacks
static std::wstring scriptName;
static std::vector<std::vector<char>> scriptArgvData;
static std::vector<char*> scriptArgvPtr;

static bool openScriptName(int argc, char* argv[])
{
    // Get script name
    if(argc < 2)
    {
        wchar_t szFileName[MAX_PATH] = L"";
        if(!OpenFileDialog(szFileName))
            return false;
        scriptName = szFileName;
    }
    else
        scriptName = Utf8ToUtf16(argv[1]);

    // Get (optional) script arguments
    scriptArgvData.clear();
    scriptArgvPtr.clear();
    if(argc > 2)
    {
        auto pyArgc = argc - 1;
        scriptArgvData.resize(pyArgc);
        scriptArgvPtr.resize(pyArgc);
        for(int i = 0; i < pyArgc; i++)
        {
            auto arg = argv[i + 1];
            scriptArgvData[i] = std::vector<char>(arg, arg + strlen(arg) + 1);
            scriptArgvPtr[i] = scriptArgvData[i].data();
        }
    }
    return true;
}

static bool cbPythonCommand(int argc, char* argv[])
{
    if(argc < 2)
    {
        _plugin_logputs("[PYTHON] Command Example: Python \"print('Hello World')\".");
        return false;
    }
    PyRun_SimpleString(argv[0] + 7);
    GuiFlushLog();
    GuiUpdateAllViews();
    return true;
}

static bool cbPipCommand(int argc, char* argv[])
{
    PyObject* pUtilsModule, *pFunc;
    PyObject* pKwargs, /* *pArgs, */ *pValue;

    if(argc < 2)
    {
        _plugin_logputs("[PYTHON] Command Example: Pip freeze");
        return false;
    }

    // Import utils
    pUtilsModule = PyObject_GetAttrString(pModule, "utils");
    if(pEventObject == NULL)
    {
        _plugin_logputs("[PYTHON] Could not find utils package.");
        PyErr_PrintEx(0);
        return false;
    }

    pFunc = PyObject_GetAttrString(pUtilsModule, "x64dbg_pip");
    if(pFunc && PyCallable_Check(pFunc))
    {
        pKwargs = Py_BuildValue("{s:s}", "args", argv[0]);
        pValue = PyObject_Call(pFunc, PyTuple_New(0), pKwargs);
        Py_DECREF(pKwargs);
        Py_DECREF(pFunc);
        if(pValue == NULL)
        {
            _plugin_logputs("[PYTHON] Could not use x64dbg_pip function.");
            PyErr_PrintEx(0);
            return false;
        }
        Py_DECREF(pValue);
    }
    return true;
}

static bool cbPyRunScriptCommand(int argc, char* argv[])
{
    if(!openScriptName(argc, argv))
        return false;
    _plugin_logprintf("[PYTHON] Executing script: \"%s\"\n", Utf16ToUtf8(scriptName).c_str());
    return ExecutePythonScript(scriptName.c_str(), int(scriptArgvPtr.size()), scriptArgvPtr.data());
}

static bool cbPyRunScriptAsyncCommand(int argc, char* argv[])
{
    if(!openScriptName(argc, argv))
        return false;
    CloseHandle(CreateThread(nullptr, 0, [](void*) -> DWORD
    {
        _plugin_logprintf("[PYTHON] Executing script: \"%s\"\n", Utf16ToUtf8(scriptName).c_str());
        ExecutePythonScript(scriptName.c_str(), int(scriptArgvPtr.size()), scriptArgvPtr.data());
        return 0;
    }, nullptr, 0, nullptr));
    return true;
}

static bool cbPyRunGuiScriptCommand(int argc, char* argv[])
{
    if(!openScriptName(argc, argv))
        return false;
    GuiExecuteOnGuiThread([]()
    {
        _plugin_logprintf("[PYTHON] Executing script: \"%s\"\n", Utf16ToUtf8(scriptName).c_str());
        ExecutePythonScript(scriptName.c_str(), int(scriptArgvPtr.size()), scriptArgvPtr.data());
    });
    return true;
}

static bool cbPythonCommandExecute(const char* cmd)
{
    if(cmd)
    {
        PyRun_SimpleString(cmd);
        GuiFlushLog();
        GuiUpdateAllViews();
        return true;
    }
    return false;
}

static void cbWinEventCallback(CBTYPE cbType, void* info)
{
    MSG* msg = ((PLUG_CB_WINEVENT*)info)->message;
    switch(msg->message)
    {
    case WM_SYSKEYDOWN:
        // Hotkeys
        switch(msg->lParam)
        {
        case ALT_F7_SYSKEYDOWN:
            DbgCmdExec("PyRunGuiScript");
            break;
        }
        break;
    }
}

static void cbInitDebugCallback(CBTYPE cbType, void* info)
{
    WIN32_FIND_DATAW FindFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    wchar_t autorunDirectory[MAX_PATH], currentDirectory[MAX_PATH];

    // Get Autorun Folder Path
    GetModuleFileNameW(NULL, autorunDirectory, MAX_PATH);
    PathRemoveFileSpecW(autorunDirectory);
    PathAppendW(autorunDirectory, autorun_directory);

    // Get Current Directory
    GetCurrentDirectoryW(MAX_PATH, currentDirectory);

    // Find And Execute *.py Files
    SetCurrentDirectoryW(autorunDirectory);
    hFind = FindFirstFileW(L"*.py", &FindFileData);
    if(hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            _plugin_logprintf("[PYTHON] Executing autorun file: \"%s\".\n", Utf16ToUtf8(FindFileData.cFileName).c_str());
            ExecutePythonScript(FindFileData.cFileName, int(scriptArgvPtr.size()), scriptArgvPtr.data());
        }
        while(FindNextFileW(hFind, &FindFileData) != 0);
        FindClose(hFind);
    }

    // Reset Current Directory
    SetCurrentDirectoryW(currentDirectory);
}

static void cbUnloadDllCallback(CBTYPE cbType, void* info)
{
    LPUNLOAD_DLL_DEBUG_INFO UnloadDll = ((PLUG_CB_UNLOADDLL*)info)->UnloadDll;

    pyCallback("unload_dll", Py_BuildValue(
                   "{s:N}",
                   "lpBaseOfDll", PyInt_FromSize_t((size_t)UnloadDll->lpBaseOfDll)
               ));
}

static void cbLoadDllCallback(CBTYPE cbType, void* info)
{
    PyObject* pLoadDll, *pPdbSig70, *pModInfo;

    PLUG_CB_LOADDLL* callbackInfo = (PLUG_CB_LOADDLL*)info;
    LOAD_DLL_DEBUG_INFO* LoadDll = callbackInfo->LoadDll;
    IMAGEHLP_MODULE64* modInfo = callbackInfo->modInfo;
    GUID PdbSig70 = modInfo->PdbSig70;

    pLoadDll = Py_BuildValue(
                   "{s:N, s:N, s:k, s:k, s:N, s:H}",
                   "hFile", PyInt_FromSize_t((size_t)LoadDll->hFile),
                   "lpBaseOfDll", PyInt_FromSize_t((size_t)LoadDll->lpBaseOfDll),
                   "dwDebugInfoFileOffset", LoadDll->dwDebugInfoFileOffset,
                   "nDebugInfoSize", LoadDll->nDebugInfoSize,
                   "lpImageName", PyInt_FromSize_t((size_t)LoadDll->lpImageName),
                   "fUnicode", LoadDll->fUnicode
               );
    pPdbSig70 = Py_BuildValue(
                    "{s:k, s:H, s:H, s:N}",
                    "Data1", PdbSig70.Data1,
                    "Data2", PdbSig70.Data2,
                    "Data3", PdbSig70.Data3,
                    "Data4", PyByteArray_FromStringAndSize(
                        (char*)PdbSig70.Data4, ARRAYSIZE(PdbSig70.Data4)
                    )
                );
    pModInfo = Py_BuildValue(
                   "{s:k, s:K, s:k, s:k, s:k, s:k, s:i, s:s, s:s, s:s, s:s, "
                   " s:k, s:s, s:k, s:N, s:k, s:N, s:N, s:N, s:N, s:N, s:N, s:N}",
                   "SizeOfStruct", modInfo->SizeOfStruct,
                   "BaseOfImage", modInfo->BaseOfImage,
                   "ImageSize", modInfo->TimeDateStamp,
                   "TimeDateStamp", modInfo->TimeDateStamp,
                   "CheckSum", modInfo->CheckSum,
                   "NumSyms", modInfo->NumSyms,
                   "SymType", modInfo->SymType,
                   "ModuleName", modInfo->ModuleName,
                   "ImageName", modInfo->ImageName,
                   "LoadedImageName", modInfo->LoadedImageName,
                   "LoadedPdbName", modInfo->LoadedPdbName,
                   "CVSig", modInfo->CVSig,
                   "CVData", modInfo->CVData,
                   "PdbSig", modInfo->PdbSig,
                   "PdbSig70", pPdbSig70,
                   "PdbAge", modInfo->PdbAge,
                   "PdbUnmatched", PyBool_FromLong(modInfo->PdbUnmatched),
                   "DbgUnmatched", PyBool_FromLong(modInfo->DbgUnmatched),
                   "LineNumbers", PyBool_FromLong(modInfo->LineNumbers),
                   "GlobalSymbols", PyBool_FromLong(modInfo->GlobalSymbols),
                   "TypeInfo", PyBool_FromLong(modInfo->TypeInfo),
                   "SourceIndexed", PyBool_FromLong(modInfo->SourceIndexed),
                   "Publics", PyBool_FromLong(modInfo->Publics)
               );
    pyCallback("load_dll", Py_BuildValue(
                   "{s:N, s:N, s:s}",
                   "LoadDll", pLoadDll,
                   "modInfo", pModInfo,
                   "modname", callbackInfo->modname
               ));
    Py_DECREF(pLoadDll);
    Py_DECREF(pPdbSig70);
    Py_DECREF(pModInfo);
}

static void cbSystemBreakpointCallback(CBTYPE cbType, void* info)
{
    pyCallback("system_breakpoint", PyDict_New());
}

static void cbExitThreadCallback(CBTYPE cbType, void* info)
{
    PLUG_CB_EXITTHREAD* callbackInfo = ((PLUG_CB_EXITTHREAD*)info);

    pyCallback("exit_thread", Py_BuildValue(
                   "{s:k, s:k}",
                   "dwThreadId", callbackInfo->dwThreadId,
                   "dwExitCode", callbackInfo->ExitThread->dwExitCode
               ));
}

static void cbCreateThreadCallback(CBTYPE cbType, void* info)
{
    PyObject* pCreateThread;

    PLUG_CB_CREATETHREAD* callbackInfo = (PLUG_CB_CREATETHREAD*)info;
    CREATE_THREAD_DEBUG_INFO* CreateThread = callbackInfo->CreateThread;

    pCreateThread = Py_BuildValue(
                        "{s:k, s:N, s:N}",
                        "hThread", CreateThread->hThread,
                        "lpThreadLocalBase", PyInt_FromSize_t((size_t)CreateThread->lpThreadLocalBase),
                        "lpStartAddress", PyInt_FromSize_t((size_t)CreateThread->lpThreadLocalBase)
                    );
    pyCallback("create_thread", Py_BuildValue(
                   "{s:k, s:N}",
                   "dwThreadId", callbackInfo->dwThreadId,
                   "CreateThread", pCreateThread
               ));
    Py_DECREF(pCreateThread);

}

static void cbExitProcessCallback(CBTYPE cbType, void* info)
{
    EXIT_PROCESS_DEBUG_INFO* ExitProcess = ((PLUG_CB_EXITPROCESS*)info)->ExitProcess;

    pyCallback("exit_process", Py_BuildValue(
                   "{s:k}",
                   "dwExitCode", ExitProcess->dwExitCode
               ));
}

static void cbCreateProcessCallback(CBTYPE cbType, void* info)
{
    PyObject* pCreateProcessInfo, *pPdbSig70, *pModInfo, *pFdProcessInfo;

    PLUG_CB_CREATEPROCESS* callbackInfo = (PLUG_CB_CREATEPROCESS*)info;
    CREATE_PROCESS_DEBUG_INFO* CreateProcessInfo = callbackInfo->CreateProcessInfo;
    IMAGEHLP_MODULE64* modInfo = callbackInfo->modInfo;
    PROCESS_INFORMATION* fdProcessInfo = callbackInfo->fdProcessInfo;
    GUID PdbSig70 = modInfo->PdbSig70;

    pCreateProcessInfo = Py_BuildValue(
                             "{s:N, s:N, s:N, s:N, s:k, s:k, s:N, s:N, s:N, s:H}",
                             "hFile", PyInt_FromSize_t((size_t)CreateProcessInfo->hFile),
                             "hProcess", PyInt_FromSize_t((size_t)CreateProcessInfo->hProcess),
                             "hThread", PyInt_FromSize_t((size_t)CreateProcessInfo->hThread),
                             "lpBaseOfImage", PyInt_FromSize_t((size_t)CreateProcessInfo->lpBaseOfImage),
                             "dwDebugInfoFileOffset", CreateProcessInfo->dwDebugInfoFileOffset,
                             "nDebugInfoSize", CreateProcessInfo->nDebugInfoSize,
                             "lpThreadLocalBase", PyInt_FromSize_t((size_t)CreateProcessInfo->lpThreadLocalBase),
                             "lpStartAddress", PyInt_FromSize_t((size_t)CreateProcessInfo->lpStartAddress),
                             "lpImageName", PyInt_FromSize_t((size_t)CreateProcessInfo->lpImageName),
                             "fUnicode", CreateProcessInfo->fUnicode
                         );
    pPdbSig70 = Py_BuildValue(
                    "{s:k, s:H, s:H, s:N}",
                    "Data1", PdbSig70.Data1,
                    "Data2", PdbSig70.Data2,
                    "Data3", PdbSig70.Data3,
                    "Data4", PyByteArray_FromStringAndSize(
                        (char*)PdbSig70.Data4, ARRAYSIZE(PdbSig70.Data4)
                    )
                );
    pModInfo = Py_BuildValue(
                   "{s:k, s:K, s:k, s:k, s:k, s:k, s:i, s:s, s:s, s:s, s:s, "
                   " s:k, s:s, s:k, s:N, s:k, s:N, s:N, s:N, s:N, s:N, s:N, s:N}",
                   "SizeOfStruct", modInfo->SizeOfStruct,
                   "BaseOfImage", modInfo->BaseOfImage,
                   "ImageSize", modInfo->TimeDateStamp,
                   "TimeDateStamp", modInfo->TimeDateStamp,
                   "CheckSum", modInfo->CheckSum,
                   "NumSyms", modInfo->NumSyms,
                   "SymType", modInfo->SymType,
                   "ModuleName", modInfo->ModuleName,
                   "ImageName", modInfo->ImageName,
                   "LoadedImageName", modInfo->LoadedImageName,
                   "LoadedPdbName", modInfo->LoadedPdbName,
                   "CVSig", modInfo->CVSig,
                   "CVData", modInfo->CVData,
                   "PdbSig", modInfo->PdbSig,
                   "PdbSig70", pPdbSig70,
                   "PdbAge", modInfo->PdbAge,
                   "PdbUnmatched", PyBool_FromLong(modInfo->PdbUnmatched),
                   "DbgUnmatched", PyBool_FromLong(modInfo->DbgUnmatched),
                   "LineNumbers", PyBool_FromLong(modInfo->LineNumbers),
                   "GlobalSymbols", PyBool_FromLong(modInfo->GlobalSymbols),
                   "TypeInfo", PyBool_FromLong(modInfo->TypeInfo),
                   "SourceIndexed", PyBool_FromLong(modInfo->SourceIndexed),
                   "Publics", PyBool_FromLong(modInfo->Publics)
               );
    pFdProcessInfo = Py_BuildValue(
                         "{s:N, s:N, s:k, s:k}",
                         "hProcess", PyInt_FromSize_t((size_t)fdProcessInfo->hProcess),
                         "hThread", PyInt_FromSize_t((size_t)fdProcessInfo->hThread),
                         "dwProcessId", fdProcessInfo->dwProcessId,
                         "dwThreadId", fdProcessInfo->dwThreadId
                     );
    pyCallback("create_process", Py_BuildValue(
                   "{s:N, s:N, s:s, s:N}",
                   "CreateProcessInfo", pCreateProcessInfo,
                   "modInfo", pModInfo,
                   "DebugFileName", callbackInfo->DebugFileName,
                   "fdProcessInfo", pFdProcessInfo
               ));
    Py_DECREF(pCreateProcessInfo);
    Py_DECREF(pPdbSig70);
    Py_DECREF(pModInfo);
    Py_DECREF(pFdProcessInfo);
}

static void cbBreakPointCallback(CBTYPE cbType, void* info)
{
    BRIDGEBP* breakpoint = ((PLUG_CB_BREAKPOINT*)info)->breakpoint;

    pyCallback("breakpoint", Py_BuildValue(
                   "{s:i, s:N, s:N, s:N, s:N, s:s, s:s, s:i}",
                   "type", breakpoint->type,
                   "addr", PyInt_FromSize_t(breakpoint->addr),
                   "enabled", PyBool_FromLong(breakpoint->enabled),
                   "singleshoot", PyBool_FromLong(breakpoint->singleshoot),
                   "active", PyBool_FromLong(breakpoint->active),
                   "mod", breakpoint->mod,
                   "name", breakpoint->name,
                   "slot", breakpoint->slot
               ));
}

static void cbStopDebugCallback(CBTYPE cbType, void* info)
{
    pyCallback("stop_debug", PyDict_New());
}

static void cbTraceExecuteCallback(CBTYPE cbType, void* info)
{
    PLUG_CB_TRACEEXECUTE* traceInfo = (PLUG_CB_TRACEEXECUTE*)info;

    PyObject* pTraceExecute;

    pTraceExecute = Py_BuildValue(
                        "{s:N, s:N}",
                        "cip", PyInt_FromSize_t(traceInfo->cip),
                        "stop", PyBool_FromLong(traceInfo->stop)
                    );

    // Packed in another dict because then it is passed by reference, so "stop" can be changed by the script.
    pyCallback("trace_execute", Py_BuildValue("{s:N}", "trace", pTraceExecute));

    traceInfo->stop = !!PyObject_IsTrue(PyDict_GetItemString(pTraceExecute, "stop"));

    Py_DECREF(pTraceExecute);
}

static std::wstring makeX64dbgPackageDir(const std::wstring & directory)
{
    auto dir = directory;
    if(dir[dir.length() - 1] != L'\\')
        dir.push_back(L'\\');
    dir.append(L"Lib\\site-packages");
    return dir;
}

static bool isValidPythonHome(const wchar_t* directory)
{
    if(!directory || !*directory)
        return false;
    auto attr = GetFileAttributesW(makeX64dbgPackageDir(directory).c_str());
    if(attr == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY;
}

static bool findX64dbgPythonHome(std::wstring & home)
{
    //Get from configuration
    char setting[MAX_SETTING_SIZE] = "";
    if(BridgeSettingGet("x64dbgpy", "PythonHome", setting))
    {
        home = Utf8ToUtf16(setting);
        if(isValidPythonHome(home.c_str()))
        {
            _plugin_logputs("[PYTHON] Found valid PythonHome in the plugin settings!");
            return true;
        }
        _plugin_logprintf("[PYTHON] Found invalid PythonHome setting \"%s\"...\n", setting);
    }
    //Get from the developer environment variable
#ifdef _WIN64
    auto python27x = _wgetenv(L"PYTHON27X64");
#else
    auto python27x = _wgetenv(L"PYTHON27X86");
#endif //_WIN64
    if(isValidPythonHome(python27x))
    {
#ifdef _WIN64
        _plugin_logputs("[PYTHON] Found valid PythonHome in the PYTHON27X64 environment variable!");
#else
        _plugin_logputs("[PYTHON] Found valid PythonHome in the PYTHON27X86 environment variable!");
#endif //_WIN64
        home = python27x;
        return true;
    }
    //Get from registry
    HKEY hKey;
    wchar_t szRegHome[MAX_SETTING_SIZE] = L"";
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Python\\PythonCore\\2.7\\InstallPath", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwSize = sizeof(szRegHome);
        RegQueryValueExW(hKey, nullptr, nullptr, nullptr, LPBYTE(szRegHome), &dwSize);
        RegCloseKey(hKey);
    }
    if(isValidPythonHome(szRegHome))
    {
        _plugin_logputs("[PYTHON] Found valid PythonHome in the registry!");
        home = szRegHome;
        return true;
    }
    //Get from PYTHONHOME environment variable
    auto pythonHome = _wgetenv(L"PYTHONHOME");
    if(isValidPythonHome(pythonHome))
    {
        _plugin_logputs("[PYTHON] Found valid PythonHome in the PYTHONHOME environment variable!");
        home = pythonHome;
        return true;
    }
    return false;
}

bool pyInit(PLUG_INITSTRUCT* initStruct)
{
    // Find and set the PythonHome
    std::wstring home;
    if(!findX64dbgPythonHome(home))
    {
        _plugin_logputs("[PYTHON] Failed to find PythonHome (do you have \\Lib\\site-packages?)...");
        BridgeSettingSet("x64dbgpy", "PythonHome", "Install Python!");
        return false;
    }
    if(!home.empty() && home.back() == L'\\')
        home.pop_back();
    auto pythonDll = home + L"\\python27.dll";
    if(!FileExists(pythonDll.c_str()))
    {
        wchar_t szSystemDir[MAX_PATH] = L"";
        GetSystemDirectoryW(szSystemDir, _countof(szSystemDir));
        pythonDll = szSystemDir;
        if(!pythonDll.empty() && pythonDll.back() == L'\\')
            pythonDll.pop_back();
        pythonDll += L"\\python27.dll";
    }
    hPython27 = LoadLibraryW(pythonDll.c_str());
    if(!hPython27)
    {
        _plugin_logprintf("[PYTHON] Failed to load Python DLL: %s\n", Utf16ToUtf8(pythonDll).c_str());
        BridgeSettingSet("x64dbgpy", "PythonHome", "Install Python!");
        return false;
    }
    else
    {
        _plugin_logprintf("[PYTHON] Python DLL: %s\n", Utf16ToUtf8(pythonDll).c_str());
    }
    BridgeSettingSet("x64dbgpy", "PythonHome", Utf16ToUtf8(home).c_str());
    static wchar_t dir[65536] = L"";
    GetShortPathNameW(home.c_str(), dir, _countof(dir));
    static char PythonHomeStatic[65536] = "";
    strncpy_s(PythonHomeStatic, Utf16ToUtf8(dir).c_str(), _TRUNCATE);
    _plugin_logprintf("[PYTHON] PythonHome: \"%s\"\n", Utf16ToUtf8(home).c_str());
    Py_SetPythonHome(PythonHomeStatic);

    // Register python command handler
    SCRIPTTYPEINFO info;
    strcpy_s(info.name, "Python");
    info.id = 0;
    info.execute = cbPythonCommandExecute;
    info.completeCommand = nullptr;
    GuiRegisterScriptLanguage(&info);

    // Register commands
    auto regCmd = [](const char* command, CBPLUGINCOMMAND cbCommand)
    {
        if(!_plugin_registercommand(pluginHandle, command, cbCommand, false))
            _plugin_logputs((std::string("[PYTHON] error registering the \"") + command + std::string("\" command!")).c_str());
    };

    regCmd("Python", cbPythonCommand);
    regCmd("Pip", cbPipCommand);
    regCmd("PyRunScript", cbPyRunScriptCommand);
    regCmd("PyRunScriptAsync", cbPyRunScriptAsyncCommand);
    regCmd("PyRunGuiScript", cbPyRunGuiScriptCommand);
    regCmd("PyDebug", [](int argc, char* argv[])
    {
        PYDATA(Py_DebugFlag) = 1;
        PYDATA(Py_VerboseFlag) = 1;
        return true;
    });

    // Initialize threads & python interpreter
    PyEval_InitThreads();
    PYDATA(Py_InspectFlag) = 1;
    Py_InitializeEx(0);

    // Add 'plugins' (current directory) to sys.path
    GetCurrentDirectoryW(_countof(dir), dir);
    if(dir[wcslen(dir) - 1] != L'\\')
        wcsncat_s(dir, L"\\", _TRUNCATE);
    wcsncat_s(dir, token_paste(L, module_name), _TRUNCATE);
    GetShortPathNameW(dir, dir, _countof(dir));
    _plugin_logputs(Utf16ToUtf8(dir).c_str());
    PyList_Insert(PySys_GetObject("path"), 0, PyString_FromString(Utf16ToUtf8(dir).c_str()));

    // Import x64dbgpy
    pModule = PyImport_Import(PyString_FromString(module_name));
    if(pModule != NULL)
    {
        // Get Event Object
        pEventObject = PyObject_GetAttrString(pModule, event_object_name);
        if(pEventObject == NULL)
        {
            _plugin_logputs("[PYTHON] Could not find Event object.");
            PyErr_PrintEx(0);
        }
    }
    else
    {
        _plugin_logputs("[PYTHON] Could not import " module_name ".");
        PyErr_PrintEx(0);
    }

    PyRun_SimpleString("from " module_name " import *\n");
    return true;
}

void pyStop()
{
    // Properly ends the python environment
    Py_Finalize();
}

void pySetup()
{
    // Set Menu Icon
    ICONDATA pyIcon;
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDB_PNG1), L"PNG");
    DWORD size = SizeofResource(hInst, hRes);
    HGLOBAL hMem = LoadResource(hInst, hRes);

    pyIcon.data = LockResource(hMem);
    pyIcon.size = size;
    _plugin_menuseticon(hMenu, &pyIcon);

    FreeResource(hMem);
    _plugin_menuaddentry(hMenu, MENU_RUNGUISCRIPT, "&Open GUI Script...\tAlt+F7");
    _plugin_menuaddentry(hMenu, MENU_RUNSCRIPTASYNC, "Open Async Script...");
    _plugin_menuaddentry(hMenu, MENU_ABOUT, "&About");

    // Set Callbacks
    _plugin_registercallback(pluginHandle, CB_WINEVENT, cbWinEventCallback);
    _plugin_registercallback(pluginHandle, CB_INITDEBUG, cbInitDebugCallback);
    _plugin_registercallback(pluginHandle, CB_BREAKPOINT, cbBreakPointCallback);
    _plugin_registercallback(pluginHandle, CB_STOPDEBUG, cbStopDebugCallback);
    _plugin_registercallback(pluginHandle, CB_CREATEPROCESS, cbCreateProcessCallback);
    _plugin_registercallback(pluginHandle, CB_EXITPROCESS, cbExitProcessCallback);
    _plugin_registercallback(pluginHandle, CB_CREATETHREAD, cbCreateThreadCallback);
    _plugin_registercallback(pluginHandle, CB_EXITTHREAD, cbExitThreadCallback);
    _plugin_registercallback(pluginHandle, CB_SYSTEMBREAKPOINT, cbSystemBreakpointCallback);
    _plugin_registercallback(pluginHandle, CB_LOADDLL, cbLoadDllCallback);
    _plugin_registercallback(pluginHandle, CB_UNLOADDLL, cbUnloadDllCallback);
    _plugin_registercallback(pluginHandle, CB_TRACEEXECUTE, cbTraceExecuteCallback);
}