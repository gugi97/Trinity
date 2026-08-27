#include <Windows.h>
#include "core/mod.h"

static HMODULE g_module = nullptr;

static DWORD WINAPI MainThread(LPVOID)
{
    trinity::Mod::Get().Initialize(g_module);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        DisableThreadLibraryCalls(module);
        // Do real work off the loader lock.
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        // If lpReserved != nullptr, the process is terminating (ExitProcess) and
        // other threads/DirectX objects have already been destroyed by the OS.
        // Calling complex unhooking and COM methods inside the loader lock on process exit
        // causes STATUS_ACCESS_VIOLATION ("Unknown Hard Error").
        // Only run full shutdown on dynamic unload (FreeLibrary: lpReserved == nullptr).
        if (!lpReserved)
        {
            trinity::Mod::Get().Shutdown();
        }
        break;
    }
    return TRUE;
}
