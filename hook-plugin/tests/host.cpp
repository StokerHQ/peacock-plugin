// Plays HITMAN3.exe for the Peacock hook plugin: carries Peacock's patterns in its own image, loads
// peacock.dll through a stand-in for the bootstrap's host, sets its "server address" the way the
// game does at start-up, and checks every patch landed. With the server option on, it also waits
// for the Peacock the plugin started, prints READY and the port the game was pointed at, and exits
// when a line arrives on stdin - so the test script can sign in, then watch Peacock stop with its game.
//
// peacock_host.exe <peacock.dll> <port, 0 for automatic> <dataFolder> <server 0|1>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "stoker/hook_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    // What the scan finds: Peacock's patterns, planted at their 16-byte alignments. Filled at run
    // time, so the patterns' bytes exist nowhere else in this image.
    alignas(4096) uint8_t g_code[0x10000] = { 1 };
    // The game's server address and its two dynamic-resources flags.
    alignas(16) char g_domain[256] = {};
    alignas(16) uint8_t g_flags[32] = {};

    std::string g_process, g_peacockFolder = "Peacock", g_dataFolder;
    int64_t g_port = 80;
    int g_server = 1;
    int g_failures = 0;

#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++g_failures; } } while (0)

    uint8_t* Base() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }
    int32_t Rva(const void* p) { return static_cast<int32_t>(static_cast<const uint8_t*>(p) - Base()); }

    void Plant(size_t at, std::string_view pattern, uint8_t filler = 0x11)
    {
        size_t k = 0;
        for (size_t i = 0; i < pattern.size();)
        {
            if (pattern[i] == ' ') { ++i; continue; }
            if (pattern[i] == '?') { g_code[at + k++] = filler; ++i; continue; }
            g_code[at + k++] = static_cast<uint8_t>(std::strtoul(std::string(pattern.substr(i, 2)).c_str(), nullptr, 16));
            i += 2;
        }
    }

    void Displacement(size_t at, int end, int displacement, const void* target)
    {
        const int32_t value = Rva(target) - (Rva(g_code + at) + end);
        std::memcpy(g_code + at + displacement, &value, 4);
    }

    void PlantBuild()
    {
        std::memset(g_code, 0xCC, sizeof g_code);
        Plant(0x1004, "0f84 ? fdffff4584f6 ? ? ? fdffff");
        // The pattern leaves the jump's opcode open, so a patched game matches too; unpatched, it is jne.
        g_code[0x1004 + 9] = 0x0F;
        g_code[0x1004 + 10] = 0x85;
        Plant(0x200c, "75084883f90675eceb22");
        Plant(0x3002, "0f84c20000004584ed0f85");
        Plant(0x400e, "488905 ? ? ? ? 488d0d ? ? ? ? 4883c4205b48ff25 ? ? ? 01");
        Displacement(0x400e, 14, 10, g_domain);
        Plant(0x5008, "68747470733a2f2f7b307d00");
        Plant(0x6007, "488945d741b8 ? 000080 448945b7", 0x0B);
        Plant(0x7004, "baa7935217488d0d");
        Displacement(0x7004, 0x22, 0x1A, &g_flags[0]);
        Plant(0x8004, "ba502e23f1488d0d");
        Displacement(0x8004, 0x22, 0x1A, &g_flags[16]);
        g_flags[0] = 1;
        g_flags[16] = 1;
    }

    void STOKER_HOOK_CALL HostLog(void*, const char* message) { std::printf("  [plugin] %s\n", message); std::fflush(stdout); }

    int STOKER_HOOK_CALL HostString(void*, const char* key, char* output, uint32_t capacity)
    {
        const std::string* value = nullptr;
        if (!std::strcmp(key, "process")) value = &g_process;
        else if (!std::strcmp(key, "peacockFolder")) value = &g_peacockFolder;
        else if (!std::strcmp(key, "dataFolder")) value = &g_dataFolder;
        if (!value) return STOKER_HOOK_NOT_FOUND;
        if (value->size() + 1 > capacity) return STOKER_HOOK_INVALID_ARGUMENT;
        std::memcpy(output, value->c_str(), value->size() + 1);
        return STOKER_HOOK_OK;
    }

    int STOKER_HOOK_CALL HostInt64(void*, const char* key, int64_t* output)
    {
        if (std::strcmp(key, "port")) return STOKER_HOOK_NOT_FOUND;
        *output = g_port;
        return STOKER_HOOK_OK;
    }

    int STOKER_HOOK_CALL HostBool(void*, const char* key, int* output)
    {
        if (std::strcmp(key, "server")) return STOKER_HOOK_NOT_FOUND;
        *output = g_server;
        return STOKER_HOOK_OK;
    }

    int STOKER_HOOK_CALL HostNumber(void*, const char*, double*) { return STOKER_HOOK_NOT_FOUND; }
    int STOKER_HOOK_CALL HostHookExport(void*, const char*, const char*, void*, void**) { return STOKER_HOOK_UNSUPPORTED; }
    int STOKER_HOOK_CALL HostHookAddress(void*, void*, void*, void**) { return STOKER_HOOK_UNSUPPORTED; }

    bool Listening(uint16_t port)
    {
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        const bool ok = connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
        closesocket(s);
        return ok;
    }

    bool Bytes(size_t at, std::string_view hex)
    {
        for (size_t i = 0; i < hex.size() / 2; ++i)
            if (g_code[at + i] != static_cast<uint8_t>(std::strtoul(std::string(hex.substr(i * 2, 2)).c_str(), nullptr, 16))) return false;
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc != 5) { std::printf("usage: peacock_host <peacock.dll> <port> <dataFolder> <server 0|1>\n"); return 2; }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    g_process = std::strrchr(self, '\\') + 1;
    g_port = std::atoll(argv[2]);
    g_dataFolder = argv[3];
    g_server = std::atoi(argv[4]);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    PlantBuild();

    HMODULE plugin = LoadLibraryA(argv[1]);
    if (!plugin) { std::printf("FAIL cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    auto init = reinterpret_cast<stoker_hook_plugin_init_fn>(GetProcAddress(plugin, "stoker_hook_plugin_init"));
    if (!init) { std::printf("FAIL no stoker_hook_plugin_init\n"); return 1; }

    stoker_hook_host_v1 host{};
    host.struct_size = sizeof host;
    host.abi_version = STOKER_HOOK_PLUGIN_ABI;
    host.game_directory_utf8 = "";
    host.config_json_utf8 = "{}";
    host.log = HostLog;
    host.hook_export = HostHookExport;
    host.hook_address = HostHookAddress;
    host.config_string = HostString;
    host.config_int64 = HostInt64;
    host.config_bool = HostBool;
    host.config_number = HostNumber;
    CHECK(init(&host) == STOKER_HOOK_OK);

    // The game sets its address a moment into its start-up; the plugin must wait for it.
    Sleep(300);
    std::memcpy(g_domain, "config.hitman.io", sizeof "config.hitman.io");

    // Without a server the address is the configured port's; with one, whichever port the plugin
    // settled on.
    const std::string loopback = "127.0.0.1";
    uint16_t port = 0;
    if (g_server)
    {
        for (int i = 0; i < 1800 && std::strncmp(g_domain, loopback.c_str(), loopback.size()) != 0; ++i) Sleep(50);
        CHECK(std::strncmp(g_domain, loopback.c_str(), loopback.size()) == 0);
        const char* rest = g_domain + loopback.size();
        port = *rest == ':' ? static_cast<uint16_t>(std::atoi(rest + 1)) : 80;
        CHECK(*rest == 0 || *rest == ':');
    }
    else
    {
        const std::string expected = g_port == 0 || g_port == 80 ? loopback : loopback + ":" + std::to_string(g_port);
        for (int i = 0; i < 300 && expected != g_domain; ++i) Sleep(50);
        CHECK(expected == g_domain);
    }
    // The plugin writes the address first and the other patches just after it; the enable flag is the last.
    for (int i = 0; i < 100 && g_flags[16] != 0; ++i) Sleep(50);
    CHECK(Bytes(0x1004 + 9, "90E9"));
    CHECK(Bytes(0x200c, "9090") && Bytes(0x3002, "909090909090"));
    // Built at run time: the string as a literal would be a second match for the scan in this image.
    char http[12] = "iuuq;00|1~";
    for (char* c = http; *c; ++c) --*c;
    CHECK(std::memcmp(g_code + 0x5008, http, 11) == 0);
    CHECK(Bytes(0x6007 + 6, "0A"));
    CHECK(g_flags[0] == 0 && g_flags[16] == 0);

    if (g_server)
    {
        // The address is written at once; Peacock serves there a few seconds later, or, started again
        // on another port, the address follows it.
        bool up = false;
        for (int i = 0; i < 450 && !up; ++i)
        {
            const char* now = g_domain + loopback.size();
            port = *now == ':' ? static_cast<uint16_t>(std::atoi(now + 1)) : 80;
            if (!(up = Listening(port))) Sleep(200);
        }
        CHECK(up);
        if (g_failures == 0)
        {
            std::printf("READY %u\n", port);
            std::fflush(stdout);
            std::string line;
            std::getline(std::cin, line);
        }
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("peacock host: patched%s\n", g_server ? ", Peacock served" : "");
    return 0;
}
