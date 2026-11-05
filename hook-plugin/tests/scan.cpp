// Tests for hitman_scan.h on synthetic images: Peacock's patterns planted at aligned offsets in a
// buffer of filler, the way they sit in HITMAN3.exe. Portable; builds with MSVC and g++.

#include "../hitman_scan.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace stoker::peacock;

static int g_failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++g_failures; } } while (0)

namespace
{
    std::vector<uint8_t> Image() { return std::vector<uint8_t>(0x40000, 0xCC); }

    /// Writes `pattern` at `at`: its bytes, and `filler` for each wildcard.
    void Plant(std::vector<uint8_t>& image, size_t at, std::string_view pattern, uint8_t filler = 0x11)
    {
        size_t k = 0;
        for (size_t i = 0; i < pattern.size();)
        {
            if (pattern[i] == ' ') { ++i; continue; }
            if (pattern[i] == '?') { image[at + k++] = filler; ++i; continue; }
            image[at + k++] = detail::Hex(pattern.substr(i, 2))[0];
            i += 2;
        }
    }

    void Int32(std::vector<uint8_t>& image, size_t at, int32_t value) { std::memcpy(image.data() + at, &value, 4); }

    /// A build shaped like the current ones (3.2xx): the newer near-jump certificate pin, the 3.210
    /// auth header, the first config-domain shape, the 3.210 length, both dynamic-resources flags.
    std::vector<uint8_t> CurrentBuild()
    {
        std::vector<uint8_t> image = Image();
        Plant(image, 0x1004, "0f84 ? fdffff4584f6 ? ? ? fdffff");
        Plant(image, 0x200c, "75084883f90675eceb22");
        Plant(image, 0x3002, "0f84c20000004584ed0f85");
        Plant(image, 0x400e, "488905 ? ? ? ? 488d0d ? ? ? ? 4883c4205b48ff25 ? ? ? 01");
        Int32(image, 0x400e + 10, 0x30000 - (0x400e + 14));
        Plant(image, 0x5008, "68747470733a2f2f7b307d00");
        Plant(image, 0x6007, "488945d741b8 ? 000080 448945b7", 0x0B);
        Plant(image, 0x7004, "baa7935217488d0d");
        Int32(image, 0x7004 + 0x1A, 0x31000 - (0x7004 + 0x22));
        Plant(image, 0x8004, "ba502e23f1488d0d");
        Int32(image, 0x8004 + 0x1A, 0x32000 - (0x8004 + 0x22));
        return image;
    }
}

int main()
{
    {
        const std::vector<uint8_t> image = CurrentBuild();
        ScanFailure failure;
        const auto patches = Scan(image, failure);
        CHECK(patches.has_value());
        CHECK(failure == ScanFailure::None);
        if (patches)
        {
            CHECK(patches->certpin.size() == 1 && patches->certpin[0].rva == 0x1004 + 9);
            CHECK(patches->certpin[0].original == detail::Hex("0F85") && patches->certpin[0].patched == detail::Hex("90E9"));
            CHECK(patches->authheader.size() == 2 && patches->authheader[0].rva == 0x200c && patches->authheader[1].rva == 0x3002);
            CHECK(patches->authheader[1].patched == detail::Hex("909090909090"));
            CHECK(patches->configdomain.size() == 1 && patches->configdomain[0].rva == 0x30000 && patches->configdomain[0].domain);
            CHECK(patches->protocol.size() == 2 && patches->protocol[0].rva == 0x5008 && patches->protocol[1].rva == 0x6007 + 6);
            CHECK(patches->protocol[0].kind == PatchKind::Data && patches->protocol[0].patched.size() == 11);
            CHECK(patches->protocol[1].kind == PatchKind::Code && patches->protocol[1].original == detail::Hex("0B"));
            CHECK(patches->dynresNoForceOffline.size() == 1 && patches->dynresNoForceOffline[0].rva == 0x31000);
            CHECK(patches->dynresEnable.size() == 1 && patches->dynresEnable[0].rva == 0x32000);
        }
    }

    {
        // Already patched by the patcher or an earlier run: the auth header's patched form, and
        // "http://{0}", are recognised.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0x200c, "90904883f90675eceb22");
        Plant(image, 0x5008, "687474703a2f2f7b307d00cc");
        ScanFailure failure;
        CHECK(Scan(image, failure).has_value());
    }

    {
        // No dynamic-resources enable switch, as in builds before it: the scan still succeeds.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0x8004, "cccccccccccccccc");
        ScanFailure failure;
        const auto patches = Scan(image, failure);
        CHECK(patches.has_value() && patches->dynresEnable.empty());
    }

    {
        // Two certificate-pinning shapes at once: ambiguous, refused.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0x9003, "? 0ec746302f000000c7461803000000");
        ScanFailure failure;
        CHECK(!Scan(image, failure).has_value());
        CHECK(failure == ScanFailure::CertPin);
    }

    {
        // A pattern off its 16-byte alignment is not a match.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0x7004, "cccccccccccccccc");
        Plant(image, 0x7005, "baa7935217488d0d");
        ScanFailure failure;
        CHECK(!Scan(image, failure).has_value());
        CHECK(failure == ScanFailure::DynresForceOffline);
    }

    {
        // The same pattern twice is not "exactly once".
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0xA00e, "488905 ? ? ? ? 488d0d ? ? ? ? 4883c4205b48ff25 ? ? ? 01");
        Int32(image, 0xA00e + 10, 0x10);
        ScanFailure failure;
        CHECK(!Scan(image, failure).has_value());
        CHECK(failure == ScanFailure::ConfigDomain);
    }

    {
        // Both length shapes at once: the protocol is ambiguous.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0xB00C, "c745B7 ? 000080488d05 ? ? ? 0148894dd7");
        ScanFailure failure;
        CHECK(!Scan(image, failure).has_value());
        CHECK(failure == ScanFailure::Protocol);
    }

    {
        // The 3.30 auth header and the older short-jump certificate pin, at Peacock's alignments.
        std::vector<uint8_t> image = CurrentBuild();
        Plant(image, 0x1004, "cccccccccccccccccccccccccccccc");
        Plant(image, 0x200c, "cccccccccccccccccccc");
        Plant(image, 0x3002, "cccccccccccccccccccccc");
        Plant(image, 0x1003, "? 0ec746302f000000c7461803000000");
        Plant(image, 0xC00d, "0f85b50000004883f90675e8");
        Plant(image, 0xD003, "0f84b800000084db0f85b0000000");
        ScanFailure failure;
        const auto patches = Scan(image, failure);
        CHECK(patches.has_value());
        if (patches)
        {
            CHECK(patches->certpin[0].rva == 0x1003 && patches->certpin[0].original == detail::Hex("75"));
            CHECK(patches->authheader[0].rva == 0xC00d && patches->authheader[1].rva == 0xD003);
        }
    }

    {
        char domain[32];
        CHECK(DomainFor(80, domain, sizeof domain) == 9 && std::string(domain) == "127.0.0.1");
        CHECK(DomainFor(41580, domain, sizeof domain) == 15 && std::string(domain) == "127.0.0.1:41580");
        CHECK(DomainFor(80, domain, 5) == 0);
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("hitman_scan: all checks passed\n");
    return 0;
}
