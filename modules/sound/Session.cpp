// Windows' volume mixer keeps the level a user sets for an app under its audio session's identity, which
// for an ordinary (per-process) session includes the executable's full path. Yea runs each release from a
// folder of its own, so every update looked like a new app and lost the level. A cross-process session is
// kept by its GUID alone (its identifier reads "...|#%b{GUID}", as Windows' own system sounds' does), so
// playback joins one whose GUID comes from the executable's file name: yea.exe keeps one level across
// updates and folders, and a game keeps its own. Two Yeas at once share it, and so its slider.
#if defined(_WIN32)
#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <stdint.h>
#include <string.h>

extern const void* soundSession; // Codec.cpp: what miniaudio passes to IAudioClient::Initialize

namespace sound
{
namespace
{
GUID    gGuid;
wchar_t gName[64];
wchar_t gIcon[MAX_PATH * 4 + 4];
} // namespace

// Before the device opens: the session its playback joins.
void sessionBegin()
{
    if (soundSession)
        return;
    wchar_t     path[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4)
        return; // miniaudio's own session, as before
    path[n] = 0;
    const wchar_t* file = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/')
            file = p + 1;
    // Its name without .exe, lowercased, hashed twice into the GUID (a name-based one, marked as such).
    wchar_t  stem[64] = {};
    uint32_t len = 0;
    for (const wchar_t* p = file; *p && *p != L'.' && len < 63; ++p)
        stem[len++] = (*p >= L'A' && *p <= L'Z') ? wchar_t(*p - L'A' + L'a') : *p;
    uint64_t h[2] = { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull };
    for (int k = 0; k < 2; ++k)
    {
        const char* salt = "yea sound session ";
        for (const char* s = salt; *s; ++s)
            h[k] = (h[k] ^ uint8_t(*s)) * 0x100000001b3ull;
        for (uint32_t i = 0; i < len; ++i)
            h[k] = (h[k] ^ uint16_t(stem[i])) * 0x100000001b3ull;
    }
    memcpy(&gGuid, h, sizeof(gGuid));
    gGuid.Data3 = uint16_t((gGuid.Data3 & 0x0fff) | 0x5000); // version 5 (name-based)
    gGuid.Data4[0] = uint8_t((gGuid.Data4[0] & 0x3f) | 0x80);
    // The mixer shows a cross-process session by the name and icon it is given: "Yea" for Yea itself,
    // else the executable's name.
    const bool yea = len == 3 && stem[0] == L'y' && stem[1] == L'e' && stem[2] == L'a';
    if (yea)
        wcscpy_s(gName, L"Yea");
    else
    {
        uint32_t k = 0;
        for (const wchar_t* p = file; *p && *p != L'.' && k < 63; ++p)
            gName[k++] = *p;
        gName[k] = 0;
    }
    wcscpy_s(gIcon, path);
    wcscat_s(gIcon, L",0");
    soundSession = &gGuid;
}

// After the device opened on WASAPI: its session's name and icon in the mixer (never kept by Windows,
// so given each time).
void sessionName(void* audioClient)
{
    static const IID kSessionControl = { 0xF4B1A599, 0x7266, 0x4319, { 0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD } };
    IAudioClient*         client = (IAudioClient*)audioClient;
    IAudioSessionControl* session = nullptr;
    if (!soundSession || !client || FAILED(client->GetService(kSessionControl, (void**)&session)) || !session)
        return;
    session->SetDisplayName(gName, NULL);
    session->SetIconPath(gIcon, NULL);
    session->Release();
}
} // namespace sound
#endif
