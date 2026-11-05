// Stoker's Peacock hook plugin: runs a Peacock server beside HITMAN World of Assassination and points
// the game at it.
//
// Stoker's Peacock plugin for HITMAN World of Assassination (README.md). The patches it makes are
// PeacockPatcher's (The Peacock Project, https://github.com/thepeacockproject/Peacock), found by the
// port of its scanner in hitman_scan.h; this file is distributed under the same licence, the GNU
// Affero General Public License version 3 or later.
//
// In the game's own process, and not as a second program the way PeacockPatcher is: the plugin is
// loaded before the game's first instruction (the game is set to load its hooks at launch), so the
// server gets the longest head start, and there is nothing for the player to start.

// Before plugin_support.h includes windows.h, which would otherwise bring in winsock 1.
#define WIN32_LEAN_AND_MEAN
#include "stoker/plugin_support.h"

STOKER_PLUGIN_MANIFEST(R"json({
  "manifestVersion": 1,
  "id": "app.stokerhq.peacock",
  "name": "Peacock (HITMAN World of Assassination)",
  "type": "hook",
  "version": "1.3.0",
  "abi": 1,
  "description": "Starts the Peacock release laid into the install, with Stoker's Peacock plugin, and points HITMAN World of Assassination at it, as PeacockPatcher does: certificate pinning off, the auth header always sent, the server's address, plain HTTP, dynamic resources. Peacock replaces IOI's online services; Stoker supplies the player and their DLC.",
  "hosts": ["steamworks"],
  "config": {
    "type": "object", "additionalProperties": false,
    "properties": {
      "process": {
        "type": "string", "title": "Game executable", "default": "HITMAN3.exe",
        "description": "The executable to act in. File name only, ASCII case-insensitive.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$"
      },
      "port": {
        "type": "integer", "title": "Port", "default": 0, "minimum": 0, "maximum": 65535,
        "description": "The port Peacock listens on, on 127.0.0.1 only, and which is written into the game's server address. 0 (automatic) takes a free port on every start. A fixed port that something other than Peacock holds is replaced by a free one; a Peacock already answering there is used."
      },
      "server": {
        "type": "boolean", "title": "Start Peacock", "default": true,
        "description": "Start the Peacock in the install. Off, the game is only pointed at the port (80 when it is automatic)."
      },
      "peacockFolder": {
        "type": "string", "title": "Peacock folder", "default": "Peacock",
        "description": "The folder in the install root that the Peacock release was laid into, unchanged. Folder name only.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$"
      },
      "dataFolder": {
        "type": "string", "title": "Data folder", "default": "Stoker Peacock",
        "description": "Peacock's working folder under %LOCALAPPDATA%: its userdata (the player's progress, which a cloud-save rule syncs), options and logs. Folder name only.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$"
      }
    }
  }
})json");

#ifdef _WIN32
#include "hitman_scan.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>

#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    using namespace stoker::plugin;
    using namespace stoker::peacock;

    /// How long the game may take to set its server address before the plugin gives up: the menus
    /// load long before it, so a game that never sets it is not one this plugin can patch.
    constexpr DWORD ReadyLimitMs = 5 * 60 * 1000;
    constexpr DWORD ReadyPollMs = 50;

    /// How long the patcher watches the started Peacock for serving on its port, or exiting before it
    /// does, which is when it is started again elsewhere.
    constexpr DWORD ListenLimitMs = 60 * 1000;

    /// How often Peacock is started on another free port after it exited before listening, which is
    /// what it does when something took its port between the choice and its own bind.
    constexpr int StartAttempts = 3;

    struct Settings
    {
        /// The configured port; 0 is automatic.
        uint16_t port = 0;
        bool server = true;
        wchar_t peacockFolder[MAX_PATH] = L"Peacock";
        wchar_t dataFolder[MAX_PATH] = L"Stoker Peacock";
    };

    Settings g_settings;

    /// The port the game is pointed at, decided at init and changed only by a restart on another port.
    uint16_t g_port = 80;

    /// The Peacock this plugin started and is waiting to see listen; null when it started none.
    HANDLE g_server = nullptr;

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring Environment(const wchar_t* name)
    {
        wchar_t value[32768];
        const DWORD length = GetEnvironmentVariableW(name, value, 32768);
        return length > 0 && length < 32768 ? std::wstring(value, length) : std::wstring();
    }

    std::wstring Parent(std::wstring path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    bool IsFile(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    /// Winsock for the lifetime of the object: every function below that opens a socket holds one.
    struct Winsock
    {
        bool ready;
        Winsock() { WSADATA wsa; ready = WSAStartup(MAKEWORD(2, 2), &wsa) == 0; }
        ~Winsock() { if (ready) WSACleanup(); }
        Winsock(const Winsock&) = delete;
        Winsock& operator=(const Winsock&) = delete;
    };

    sockaddr_in Loopback(uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        return address;
    }

    /// A blocking socket connected to 127.0.0.1:port within 300 ms, with that limit on its sends and
    /// receives too; INVALID_SOCKET when nothing accepts there.
    SOCKET Connect(uint16_t port)
    {
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        const sockaddr_in address = Loopback(port);
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        fd_set writable, failed;
        FD_ZERO(&writable); FD_ZERO(&failed);
        FD_SET(s, &writable); FD_SET(s, &failed);
        timeval wait{ 0, 300 * 1000 };
        if (select(0, nullptr, &writable, &failed, &wait) <= 0 || !FD_ISSET(s, &writable))
        {
            closesocket(s);
            return INVALID_SOCKET;
        }
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        const DWORD limit = 300;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&limit), sizeof limit);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&limit), sizeof limit);
        return s;
    }

    /// Whether something accepts connections on 127.0.0.1:port.
    bool Answers(uint16_t port)
    {
        const Winsock winsock;
        const SOCKET s = winsock.ready ? Connect(port) : INVALID_SOCKET;
        if (s == INVALID_SOCKET) return false;
        closesocket(s);
        return true;
    }

    /// Whether what answers on 127.0.0.1:port is a Peacock: it serves the game's online
    /// configuration at Peacock's path, which a web server that merely holds the port - IIS, the
    /// Web Deployment Agent - does not.
    bool IsPeacock(uint16_t port)
    {
        const Winsock winsock;
        const SOCKET s = winsock.ready ? Connect(port) : INVALID_SOCKET;
        if (s == INVALID_SOCKET) return false;
        const std::string request = "GET /files/onlineconfig.json HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nConnection: close\r\n\r\n";
        char status[16] = "";
        int received = 0;
        if (send(s, request.c_str(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()))
            for (int n; received < static_cast<int>(sizeof status) - 1 &&
                        (n = recv(s, status + received, static_cast<int>(sizeof status) - 1 - received, 0)) > 0;)
                received += n;
        closesocket(s);
        status[received] = 0;
        return std::strncmp(status, "HTTP/1.1 200", 12) == 0;
    }

    /// A port nothing on 127.0.0.1 holds, as the system picks one for a bind to port 0; 0 when none
    /// could be had. The port is free when chosen, not reserved: Peacock binds it a few seconds
    /// later, and the patcher starts it on another if it was taken in between.
    uint16_t FreePort()
    {
        const Winsock winsock;
        if (!winsock.ready) return 0;
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
        const BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in address = Loopback(0);
        int size = sizeof address;
        uint16_t port = 0;
        if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 &&
            getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0)
            port = ntohs(address.sin_port);
        closesocket(s);
        return port;
    }

    /// The process's environment with HOST, PORT and STOKER_PEACOCK_GAME_PID set for Peacock, as a
    /// CreateProcessW Unicode block. Everything else - STOKER_INSTALL_DIR above all, which the
    /// Peacock plugin reads the install's DLC from - is the game's, unchanged.
    std::vector<wchar_t> ServerEnvironment(uint16_t port)
    {
        const wchar_t* ours[] = { L"HOST=", L"PORT=", L"STOKER_PEACOCK_GAME_PID=" };
        std::vector<wchar_t> block;
        if (wchar_t* strings = GetEnvironmentStringsW())
        {
            for (const wchar_t* entry = strings; *entry; entry += wcslen(entry) + 1)
            {
                bool replaced = false;
                for (const wchar_t* name : ours)
                    replaced |= _wcsnicmp(entry, name, wcslen(name)) == 0;
                if (!replaced) block.insert(block.end(), entry, entry + wcslen(entry) + 1);
            }
            FreeEnvironmentStringsW(strings);
        }
        for (const std::wstring& entry : { std::wstring(L"HOST=127.0.0.1"), L"PORT=" + std::to_wstring(port),
                                           L"STOKER_PEACOCK_GAME_PID=" + std::to_wstring(GetCurrentProcessId()) })
            block.insert(block.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
        block.push_back(L'\0');
        return block;
    }

    /// `peacock-server.log` in the install's logs folder, beside the other integrations' logs: the
    /// output of the Peacock this run started. Empty when STOKER_INSTALL_DIR is not set.
    std::wstring ServerLogPath()
    {
        const std::wstring stokerData = Environment(L"STOKER_INSTALL_DIR");
        if (stokerData.empty()) return {};
        const std::wstring logs = stokerData + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        return logs + L"\\peacock-server.log";
    }

    /// Clears the server log at the start of every run of the game, whether or not this run starts a
    /// Peacock, so the log only ever holds one run: from the game's start to Peacock's exit after it.
    /// The previous run's Peacock may still be writing its last profile into that file; the file is
    /// renamed away first, so its name is free for this run even while that handle stays open, and
    /// the old file goes once the old Peacock closes it.
    void ClearServerLog()
    {
        const std::wstring path = ServerLogPath();
        if (path.empty()) return;
        const std::wstring previous = path + L".previous";
        if (MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(previous.c_str());
        else DeleteFileW(path.c_str());
        // A server log directly in the install's data folder is an older plugin's; nothing reads it.
        DeleteFileW((Environment(L"STOKER_INSTALL_DIR") + L"\\peacock-server.log").c_str());
    }

    /// The Stoker plugin for Peacock and the launcher's protocol it loads from beside itself, in peacock.rc.
    constexpr int PluginResource = 101;
    constexpr int AgentResource = 102;

    HMODULE ThisModule()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisModule), &module);
        return module;
    }

    /// Writes one of the files this library carries (peacock.rc) to `path`, replacing what is there.
    bool WriteResource(int id, const std::wstring& path)
    {
        const HMODULE module = ThisModule();
        const HRSRC found = module ? FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)) : nullptr;   // RT_RCDATA
        const HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        const DWORD size = found ? SizeofResource(module, found) : 0;
        if (!bytes || size == 0) return false;

        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
        CloseHandle(file);
        return ok;
    }

    /// Peacock's options as a Stoker install starts with them; everything else is the release's default.
    /// `updateChecking`: a new Peacock comes as a new release laid into the install, not from Peacock's own
    /// check. `leaderboards`: off for a Peacock started without the launcher, where submitting would send
    /// the player's Steam id to Peacock's public leaderboard service; under the launcher the Stoker plugin
    /// turns it on, pointed at Stoker's boards.
    constexpr const char* OptionOverrides[][2] = { { "updateChecking", "false" }, { "leaderboards", "false" } };

    /// Writes `target` once, from the release's options.ini (`release`) - every option with its
    /// description - with OptionOverrides' values in place of the release's, and any missing appended.
    /// Left alone when it exists: from then on the options are the player's.
    void SeedOptions(const std::wstring& release, const std::wstring& target)
    {
        if (IsFile(target)) return;

        std::string text;
        HANDLE in = CreateFileW(release.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(in, &size) && size.QuadPart > 0 && size.QuadPart < 1 << 20)
            {
                text.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (!ReadFile(in, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
                text.resize(read);
            }
            CloseHandle(in);
        }

        bool seen[std::size(OptionOverrides)] = {};
        std::string out;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            size_t key = line.find_first_not_of(" \t");
            const size_t equals = line.find('=');
            for (size_t i = 0; i < std::size(OptionOverrides) && key != std::string::npos && equals != std::string::npos; ++i)
            {
                std::string name = line.substr(key, equals - key);
                name.erase(name.find_last_not_of(" \t") + 1);
                if (name == OptionOverrides[i][0])
                {
                    line = std::string(OptionOverrides[i][0]) + "=" + OptionOverrides[i][1];
                    seen[i] = true;
                    break;
                }
            }
            out += line + "\r\n";
            start = end + 1;
        }
        for (size_t i = 0; i < std::size(OptionOverrides); ++i)
            if (!seen[i]) out += std::string(OptionOverrides[i][0]) + "=" + OptionOverrides[i][1] + "\r\n";

        HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
        CloseHandle(file);
    }

    /// Starts `<install>\<peacockFolder>\nodedist\node.exe chunk0.js` with the data folder as its
    /// working folder. Peacock keeps everything it writes - userdata, options.ini, plugins, logs - in
    /// its working folder and reads its own files from beside chunk0.js, so the install stays as
    /// stored and the player's progress lives outside it, where an update or repair cannot take it.
    ///
    /// The server is the game's child, so it runs in the launcher's job for this run: the run ends,
    /// and the launcher collects the saves, only when the server has gone too. The Stoker plugin in
    /// Peacock ends the server, after writing every profile, once this process has exited.
    ///
    /// Its output is appended to the server log (ServerLogPath), which the run cleared at its start,
    /// so a later attempt on another port in the same run follows the first's. Returns the process,
    /// or null when Peacock was not started.
    HANDLE StartServer(uint16_t port)
    {
        const std::wstring stokerData = Environment(L"STOKER_INSTALL_DIR");
        if (stokerData.empty())
        {
            Log("peacock: STOKER_INSTALL_DIR is not set, so the install is unknown; not starting Peacock");
            return nullptr;
        }
        const std::wstring peacock = Parent(stokerData) + L"\\" + g_settings.peacockFolder;
        const std::wstring node = peacock + L"\\nodedist\\node.exe";
        const std::wstring script = peacock + L"\\chunk0.js";
        if (!IsFile(node) || !IsFile(script))
        {
            Log("peacock: no Peacock release at %s (nodedist\\node.exe, chunk0.js); not starting Peacock", Utf8(peacock).c_str());
            return nullptr;
        }

        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)))
        {
            Log("peacock: %%LOCALAPPDATA%% could not be found; not starting Peacock");
            return nullptr;
        }
        const std::wstring data = std::wstring(localAppData) + L"\\" + g_settings.dataFolder;
        CoTaskMemFree(localAppData);
        CreateDirectoryW(data.c_str(), nullptr);
        CreateDirectoryW((data + L"\\plugins").c_str(), nullptr);

        // The Stoker plugin is this library's, written at every start, so a new plugin version updates
        // it. The options are the player's once written, as Peacock's own are: the release's, with
        // Stoker's values, are only a first default.
        if (!WriteResource(PluginResource, data + L"\\plugins\\stoker.plugin.js") ||
            !WriteResource(AgentResource, data + L"\\plugins\\stoker-agent.js"))
        {
            Log("peacock: the Stoker plugin could not be written into %s (%lu); not starting Peacock", Utf8(data).c_str(), GetLastError());
            return nullptr;
        }
        SeedOptions(peacock + L"\\options.ini", data + L"\\options.ini");

        SECURITY_ATTRIBUTES inherit{ sizeof inherit, nullptr, TRUE };
        const std::wstring logPath = ServerLogPath();
        // Append-only access, so every write lands at the end, whichever attempt makes it.
        HANDLE log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        // Only the log handle is inherited, never the game's own inheritable handles.
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> attributes(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        const bool listed = log != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(list, 1, 0, &size) &&
            UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof log, nullptr, nullptr);

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof startup;
        if (listed)
        {
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput = log;
            startup.StartupInfo.hStdError = log;
            startup.lpAttributeList = list;
        }

        std::wstring command = L"\"" + node + L"\" \"" + script + L"\"";
        std::vector<wchar_t> environment = ServerEnvironment(port);
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(node.c_str(), command.data(), nullptr, nullptr, listed ? TRUE : FALSE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                            environment.data(), data.c_str(), &startup.StartupInfo, &process);
        const DWORD error = GetLastError();
        if (listed) DeleteProcThreadAttributeList(list);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

        if (!started)
        {
            Log("peacock: Peacock could not be started (%lu)", error);
            return nullptr;
        }
        Log("peacock: started Peacock (process %lu) on 127.0.0.1:%u, data in %s, output in %s",
            process.dwProcessId, port, Utf8(data).c_str(), Utf8(logPath).c_str());
        CloseHandle(process.hThread);
        return process.hProcess;
    }

    /// A copy of the executable's image as mapped, indexed by RVA. Pages that cannot be read without
    /// side effects - reserved, no-access, guard - stay zero: reading a guard page would take the
    /// guard off a page the game set it on.
    std::vector<uint8_t> CopyImage(const uint8_t* base)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage);
        for (size_t offset = 0; offset < image.size();)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(base + offset, &region, sizeof region)) break;
            const size_t end = (std::min)(image.size(), static_cast<size_t>(static_cast<const uint8_t*>(region.BaseAddress) + region.RegionSize - base));
            const DWORD protect = region.Protect;
            const bool readable = region.State == MEM_COMMIT && !(protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
            if (readable) std::memcpy(image.data() + offset, base + offset, end - offset);
            offset = end;
        }
        return image;
    }

    /// Reads `size` bytes at `address`. A guard on the page - the patcher allows for one on the
    /// server address - is lifted for the read and put back; nothing else about the page changes,
    /// so code that other threads are running stays executable throughout.
    bool Read(uint8_t* address, void* output, size_t size)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(address, &region, sizeof region) || region.State != MEM_COMMIT || (region.Protect & PAGE_NOACCESS)) return false;
        if (!(region.Protect & PAGE_GUARD))
        {
            std::memcpy(output, address, size);
            return true;
        }
        DWORD old = 0;
        if (!VirtualProtect(address, size, region.Protect & ~PAGE_GUARD, &old)) return false;
        std::memcpy(output, address, size);
        DWORD ignored = 0;
        VirtualProtect(address, size, old, &ignored);
        return true;
    }

    bool Write(uint8_t* address, const void* bytes, size_t size, PatchKind kind)
    {
        DWORD old = 0;
        if (!VirtualProtect(address, size, kind == PatchKind::Code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return false;
        std::memcpy(address, bytes, size);
        DWORD ignored = 0;
        VirtualProtect(address, size, old, &ignored);
        if (kind == PatchKind::Code) FlushInstructionCache(GetCurrentProcess(), address, size);
        return true;
    }

    enum class Check { Original, AlreadyPatched, Unexpected };

    Check Inspect(uint8_t* base, const Patch& patch)
    {
        std::vector<uint8_t> current((std::max)(patch.original.size(), patch.patched.size()));
        if (!Read(base + patch.rva, current.data(), current.size())) return Check::Unexpected;
        if (std::memcmp(current.data(), patch.original.data(), patch.original.size()) == 0) return Check::Original;
        if (std::memcmp(current.data(), patch.patched.data(), patch.patched.size()) == 0) return Check::AlreadyPatched;
        return Check::Unexpected;
    }

    void LogFamily(const char* name, const std::vector<Patch>& patches)
    {
        std::string offsets;
        for (const Patch& p : patches)
        {
            char rva[16];
            std::snprintf(rva, sizeof rva, "%s0x%07X", offsets.empty() ? "" : ", ", p.rva);
            offsets += rva;
        }
        Log("peacock: %s at %s", name, offsets.empty() ? "(none in this build)" : offsets.c_str());
    }

    /// Waits for the Peacock this plugin started to serve on g_port. One that exits first - which
    /// Peacock does when its port was taken between the choice and its own bind - is started again on
    /// another free port, StartAttempts times in all, and g_port follows it. Gives up watching after
    /// ListenLimitMs.
    void AwaitServer()
    {
        if (!g_server) return;
        const DWORD started = GetTickCount();
        for (int attempt = 1;;)
        {
            if (IsPeacock(g_port))
            {
                Log("peacock: Peacock serves on 127.0.0.1:%u", g_port);
                break;
            }
            if (WaitForSingleObject(g_server, 0) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                GetExitCodeProcess(g_server, &code);
                CloseHandle(g_server);
                g_server = nullptr;
                const uint16_t port = attempt < StartAttempts ? FreePort() : 0;
                if (!port)
                {
                    Log("peacock: Peacock exited (%lu) before serving on 127.0.0.1:%u; its output is in logs\\peacock-server.log", code, g_port);
                    return;
                }
                Log("peacock: Peacock exited (%lu) before serving on 127.0.0.1:%u, which something else may have taken; starting it on %u",
                    code, g_port, port);
                g_port = port;
                ++attempt;
                if (!(g_server = StartServer(port))) return;
                continue;
            }
            if (GetTickCount() - started > ListenLimitMs)
            {
                Log("peacock: Peacock does not serve on 127.0.0.1:%u after %lu seconds; no longer watching it", g_port, ListenLimitMs / 1000);
                break;
            }
            Sleep(200);
        }
        CloseHandle(g_server);
        g_server = nullptr;
    }

    DWORD WINAPI Patcher(void*)
    {
        try
        {
            auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            Log("peacock: scanning the game (PE timestamp 0x%08X, %u bytes)", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);

            // The copy is as large as the game's image; it goes as soon as the scan is done, not after the wait below.
            ScanFailure failure = ScanFailure::None;
            const std::optional<Patches> found = Scan(CopyImage(base), failure);
            if (!found)
            {
                Log("peacock: this build is not one Peacock's patcher knows: the %s patch was not found exactly once. The game stays on IOI's servers.", Name(failure));
                return 0;
            }
            LogFamily("certificate pinning", found->certpin);
            LogFamily("auth header", found->authheader);
            LogFamily("config domain", found->configdomain);
            LogFamily("protocol", found->protocol);
            LogFamily("dynamic resources (force offline)", found->dynresNoForceOffline);
            LogFamily("dynamic resources (enable)", found->dynresEnable);

            // Code is checked before anything is written, so a build that matches the patterns but
            // not the bytes is left whole rather than half patched. Data is written as the patcher
            // writes it, unchecked: the dynamic-resources flags are the game's runtime state, which
            // it may not have set yet, and the protocol string is the one the scan just matched.
            std::vector<const Patch*> writes;
            for (const std::vector<Patch>* family : { &found->certpin, &found->authheader, &found->protocol, &found->dynresNoForceOffline, &found->dynresEnable })
                for (const Patch& patch : *family)
                {
                    if (patch.kind == PatchKind::Data) { writes.push_back(&patch); continue; }
                    const Check check = Inspect(base, patch);
                    if (check == Check::Unexpected)
                    {
                        Log("peacock: the bytes at 0x%07X are not what Peacock's patcher expects; nothing is patched", patch.rva);
                        return 0;
                    }
                    if (check == Check::Original) writes.push_back(&patch);
                }

            // The game sets its server address at start-up, after the image is loaded: wait for it,
            // then overwrite it, as the patcher does.
            uint8_t* domain = base + found->configdomain[0].rva;
            const DWORD started = GetTickCount();
            for (uint8_t first = 0;;)
            {
                if (!Read(domain, &first, 1)) { Log("peacock: the server address cannot be read"); return 0; }
                if (first) break;
                if (GetTickCount() - started > ReadyLimitMs) { Log("peacock: the game never set its server address; nothing is patched"); return 0; }
                Sleep(ReadyPollMs);
            }

            char address[32];
            const size_t length = DomainFor(g_port, address, sizeof address);
            char previous[64] = "";
            Read(domain, previous, sizeof previous - 1);
            if (!Write(domain, address, length + 1, PatchKind::Data)) { Log("peacock: the server address could not be written (%lu)", GetLastError()); return 0; }
            for (const Patch* patch : writes)
                if (!Write(base + patch->rva, patch->patched.data(), patch->patched.size(), patch->kind))
                    Log("peacock: the patch at 0x%07X could not be written (%lu)", patch->rva, GetLastError());

            Log("peacock: the game now talks to http://%s (was %s), %zu patches written", address, previous, writes.size());

            // Patched as soon as the game sets its address, so its first request cannot go to IOI; the
            // address is written again if Peacock had to move to another port to serve.
            const uint16_t written = g_port;
            AwaitServer();
            if (g_port != written)
            {
                const size_t moved = DomainFor(g_port, address, sizeof address);
                if (Write(domain, address, moved + 1, PatchKind::Data)) Log("peacock: the game now talks to http://%s", address);
                else Log("peacock: the server address could not be written again (%lu)", GetLastError());
            }
        }
        catch (...)
        {
            Log("peacock: patching failed unexpectedly");
        }
        return 0;
    }

    bool FolderOption(const char* key, wchar_t* output)
    {
        char value[MAX_PATH] = "";
        const OptionRead read = Option(key, value, sizeof value);
        if (read == OptionRead::Missing) return true;
        return read == OptionRead::Present && PlainFileName(value) && Wide(value, output, MAX_PATH);
    }
}
#endif

STOKER_HOOK_PLUGIN_ENTRY int STOKER_HOOK_CALL stoker_hook_plugin_init(const stoker_hook_host_v1* host)
{
    if (!stoker::plugin::Accept(host)) return STOKER_HOOK_UNSUPPORTED;
#ifdef _WIN32
    try
    {
        using namespace stoker::plugin;

        // Absent means HITMAN3.exe, not every process: the plugin is delivered beside every
        // Steamworks site of the build, and only the game has anything to patch.
        char process[MAX_PATH] = "HITMAN3.exe";
        wchar_t wanted[MAX_PATH], self[MAX_PATH * 2];
        if (Option("process", process, sizeof process) == OptionRead::Invalid || !PlainFileName(process) || !Wide(process, wanted, MAX_PATH))
            return STOKER_HOOK_INVALID_ARGUMENT;
        const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        if (length == 0 || length >= MAX_PATH * 2) return STOKER_HOOK_ERROR;
        if (!NamesFile(self, wanted)) { Log("peacock: not %s; nothing to do in this process", process); return STOKER_HOOK_OK; }

        if (STOKER_HOOK_HAS(host, config_int64))
        {
            int64_t port = 0;
            const int read = host->config_int64(host->user, "port", &port);
            if (read == STOKER_HOOK_INVALID_ARGUMENT || port < 0 || port > 65535) return STOKER_HOOK_INVALID_ARGUMENT;
            g_settings.port = static_cast<uint16_t>(port);
        }
        if (STOKER_HOOK_HAS(host, config_bool))
        {
            int server = 1;
            if (host->config_bool(host->user, "server", &server) == STOKER_HOOK_INVALID_ARGUMENT) return STOKER_HOOK_INVALID_ARGUMENT;
            g_settings.server = server != 0;
        }
        if (!FolderOption("peacockFolder", g_settings.peacockFolder) || !FolderOption("dataFolder", g_settings.dataFolder))
            return STOKER_HOOK_INVALID_ARGUMENT;

        ClearServerLog();

        // The port is settled here, before the game runs: automatic takes a free one; a fixed one is
        // kept when it is free or a Peacock already serves there, and otherwise traded for a free one.
        g_port = g_settings.port;
        if (!g_settings.server)
        {
            if (!g_port) g_port = 80;
            Log("peacock: starting Peacock is switched off; pointing the game at port %u", g_port);
        }
        else if (g_port && IsPeacock(g_port))
            Log("peacock: a Peacock already serves on 127.0.0.1:%u; using it rather than starting one", g_port);
        else
        {
            if (g_port && Answers(g_port))
            {
                Log("peacock: something that is not Peacock holds 127.0.0.1:%u; starting Peacock on a free port instead", g_port);
                g_port = 0;
            }
            if (!g_port && !(g_port = FreePort()))
            {
                Log("peacock: no free port on 127.0.0.1 could be found; not starting Peacock");
                return STOKER_HOOK_ERROR;
            }
            g_server = StartServer(g_port);
        }

        // Scanning and waiting take a while; the game's start-up must not wait for them.
        HANDLE thread = CreateThread(nullptr, 0, &Patcher, nullptr, 0, nullptr);
        if (!thread) { Log("peacock: the patching thread could not be started (%lu)", GetLastError()); return STOKER_HOOK_ERROR; }
        CloseHandle(thread);
        return STOKER_HOOK_OK;
    }
    catch (...) { return STOKER_HOOK_ERROR; }
#else
    host->log(host->user, "peacock: HITMAN World of Assassination is a Windows game; there is no native Linux build to patch");
    return STOKER_HOOK_UNSUPPORTED;
#endif
}
