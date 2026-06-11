/*
Census - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * CensusHttp - isolated WinHTTP transport for Census.
 * Compiled separately from Ark.h to avoid winhttp.h / UE header collisions.
 */

#include <Windows.h>
#include <winhttp.h>
#include <string>

#pragma comment(lib, "winhttp")

static std::wstring CensusWiden(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

bool CensusHttpSend(const std::string& verb, const std::string& url,
    const std::string& body, std::string& outResp, int& outStatus)
{
    outResp.clear();
    outStatus = 0;

    std::wstring wurl = CensusWiden(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName = hostBuf; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pathBuf;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    std::wstring wverb = CensusWiden(verb);

    HINTERNET hSession = WinHttpOpen(L"Census/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, wverb.c_str(), path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hRequest)
        {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD code = 0, codeSize = sizeof(code);
                WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX);
                outStatus = (int)code;

                DWORD avail = 0;
                do
                {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
                    if (avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
                    chunk.resize(read);
                    outResp += chunk;
                } while (avail > 0);

                ok = true;
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}