// Finds, in HITMAN World of Assassination's executable image, the places Peacock patches to point the
// game at a Peacock server.
//
// A port of PeacockPatcher's AOBScanner (patcher/HitmanPatcher.Core/AOBScanner.cs in The Peacock
// Project, https://github.com/thepeacockproject/Peacock), and distributed under its licence:
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU Affero General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public License
//     along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// The patterns, their alignments, the address arithmetic and the "exactly one match" rules are
// Peacock's, unchanged, so a build Peacock's patcher can patch is a build this can patch. Peacock's
// per-version offset table is not ported: it ends at 3.120, and the patcher finds every later build,
// the ones in the field, by this scan.
//
// Portable C++20 with no OS calls: the caller hands in a copy of the image, indexed by RVA.

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace stoker::peacock
{
    /// What a patch is for, which decides how its page has to be opened to write it.
    enum class PatchKind : uint8_t { Code, Data };

    /// One write: at `rva`, `original` is expected and `patched` is written. The config domain has
    /// neither - its value is the server's address, written by the caller (`domain`).
    struct Patch
    {
        uint32_t rva = 0;
        std::vector<uint8_t> original;
        std::vector<uint8_t> patched;
        PatchKind kind = PatchKind::Code;
        bool domain = false;
    };

    /// Every patch Peacock makes: certificate pinning off, the auth header always sent, the server's
    /// address, plain HTTP, and the dynamic-resources switches. `dynresEnable` is empty in builds
    /// that have no such switch; every other family has at least one patch when the scan succeeds.
    struct Patches
    {
        std::vector<Patch> certpin, authheader, configdomain, protocol, dynresNoForceOffline, dynresEnable;
    };

    /// The family that was not found exactly once, for the log.
    enum class ScanFailure : uint8_t { None, CertPin, AuthHeader, ConfigDomain, Protocol, DynresForceOffline };

    inline const char* Name(ScanFailure failure)
    {
        switch (failure)
        {
        case ScanFailure::CertPin: return "certificate pinning";
        case ScanFailure::AuthHeader: return "auth header";
        case ScanFailure::ConfigDomain: return "config domain";
        case ScanFailure::Protocol: return "protocol";
        case ScanFailure::DynresForceOffline: return "dynamic resources (force offline)";
        default: return "none";
        }
    }

    namespace detail
    {
        inline std::vector<uint8_t> Hex(std::string_view hex)
        {
            auto nibble = [](char c) -> uint8_t
            {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
                return static_cast<uint8_t>(c - 'A' + 10);
            };
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
            return bytes;
        }

        inline std::vector<uint8_t> Text(std::string_view text)
        {
            std::vector<uint8_t> bytes(text.begin(), text.end());
            bytes.push_back(0);
            return bytes;
        }

        /// Every start offset of `pattern` in `data`, at offsets congruent to `alignment` modulo 16.
        /// The pattern is hex pairs with `?` for one byte of anything, spaces ignored - the
        /// patcher's findPattern, whose stride of 16 is why each pattern carries its alignment: the
        /// functions it looks for start on 16-byte boundaries, and the alignment is where in its
        /// 16 bytes the pattern begins.
        inline std::vector<uint32_t> Find(std::span<const uint8_t> data, uint8_t alignment, std::string_view pattern)
        {
            std::vector<int> bytes; // -1 is a wildcard
            for (size_t i = 0; i < pattern.size();)
            {
                if (pattern[i] == ' ') { ++i; continue; }
                if (pattern[i] == '?') { bytes.push_back(-1); ++i; continue; }
                bytes.push_back(Hex(pattern.substr(i, 2))[0]);
                i += 2;
            }

            std::vector<uint32_t> found;
            if (bytes.empty() || data.size() < bytes.size()) return found;
            const size_t last = data.size() - bytes.size();
            for (size_t start = alignment; start <= last; start += 16)
            {
                bool match = true;
                for (size_t k = 0; k < bytes.size() && match; ++k)
                    match = bytes[k] < 0 || data[start + k] == bytes[k];
                if (match) found.push_back(static_cast<uint32_t>(start));
            }
            return found;
        }

        inline std::optional<int32_t> Int32At(std::span<const uint8_t> data, size_t offset)
        {
            if (offset + 4 > data.size()) return std::nullopt;
            int32_t value;
            std::memcpy(&value, data.data() + offset, 4);
            return value;
        }

        /// The patcher's RIP-relative resolution: for each match at `address`, the target is
        /// `address + end + int32 at (address + displacement)`.
        inline std::vector<uint32_t> Targets(std::span<const uint8_t> data, const std::vector<uint32_t>& matches, int end, int displacement)
        {
            std::vector<uint32_t> targets;
            for (const uint32_t address : matches)
                if (const auto relative = Int32At(data, address + static_cast<size_t>(displacement)))
                    targets.push_back(static_cast<uint32_t>(static_cast<int64_t>(address) + end + *relative));
            return targets;
        }

        inline void Append(std::vector<uint32_t>& to, const std::vector<uint32_t>& from) { to.insert(to.end(), from.begin(), from.end()); }

        inline Patch Code(uint32_t rva, std::string_view original, std::string_view patched)
        {
            return { rva, Hex(original), Hex(patched), PatchKind::Code, false };
        }

        inline Patch Data(uint32_t rva, std::string_view original, std::string_view patched)
        {
            return { rva, Hex(original), Hex(patched), PatchKind::Data, false };
        }

        using Candidate = std::optional<std::vector<Patch>>;

        // certpin

        inline Candidate CertpinNearJump(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Find(d, 0xe, "? ? 9afdffffc747302f000000c7471803000000");
            Append(offsets, Find(d, 0xc, "? ? 9afdffffc747302f000000c7471803000000")); // 1.15
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0], "0F85", "90E9") };
        }

        inline Candidate CertpinNearJumpNew(std::span<const uint8_t> d)
        {
            const auto offsets = Find(d, 0x4, "0f84 ? fdffff4584f6 ? ? ? fdffff");
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0] + 9, "0F85", "90E9") };
        }

        inline Candidate CertpinShortJump(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Find(d, 0x3, "? 0ec746302f000000c7461803000000");
            Append(offsets, Find(d, 0x2, "? 0ec746302f000000c7461803000000")); // 2.13
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0], "75", "EB") };
        }

        // authheader: each pattern is searched unpatched and patched, so a game the patcher (or
        // this plugin) already patched is still recognised.

        inline Candidate AuthHeader3_210(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> first = Find(d, 0xc, "75084883f90675eceb22");
            Append(first, Find(d, 0xc, "90904883f90675eceb22"));
            std::vector<uint32_t> second = Find(d, 0x2, "0f84c20000004584ed0f85");
            Append(second, Find(d, 0x2, "9090909090904584ed0f85"));
            if (first.size() != 1 || second.size() != 1) return std::nullopt;
            return std::vector{ Code(first[0], "7508", "9090"), Code(second[0], "0F84C2000000", "909090909090") };
        }

        inline Candidate AuthHeader3_30(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> first = Find(d, 0xd, "0f85b50000004883f90675e8");
            Append(first, Find(d, 0xd, "9090909090904883f90675e8"));
            std::vector<uint32_t> second = Find(d, 0x3, "0f84b800000084db0f85b0000000");
            Append(second, Find(d, 0x3, "90909090909084db0f85b0000000"));
            if (first.size() != 1 || second.size() != 1) return std::nullopt;
            return std::vector{ Code(first[0], "0F85B5000000", "909090909090"), Code(second[0], "0F84B8000000", "909090909090") };
        }

        inline Candidate AuthHeader2_72(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> first = Find(d, 0x8, "? 18488d15ff02cd00");
            Append(first, Find(d, 0x8, "? 18488d155fd6cc00"));
            Append(first, Find(d, 0x7, "? 18488d152018cc00")); // 2.13
            std::vector<uint32_t> second = Find(d, 0xc, "0f84860000004584e4");
            Append(second, Find(d, 0xc, "9090909090904584e4"));
            Append(second, Find(d, 0xb, "0f84830000004584e4")); // 2.13
            Append(second, Find(d, 0xb, "9090909090904584e4")); // 2.13
            if (first.size() != 1 || second.size() != 1) return std::nullopt;
            return std::vector{ Code(first[0], "75", "EB"), Code(second[0], "0F8486000000", "909090909090") };
        }

        inline Candidate AuthHeader1_15(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> first = Find(d, 0x5, "0f84b3000000498bcf");
            Append(first, Find(d, 0x5, "909090909090498bcf"));
            std::vector<uint32_t> second = Find(d, 0x5, "0f84a30000004584ed");
            Append(second, Find(d, 0x5, "9090909090904584ed"));
            if (first.size() != 1 || second.size() != 1) return std::nullopt;
            return std::vector{ Code(first[0], "0F84B3000000", "909090909090"), Code(second[0], "0F84A3000000", "909090909090") };
        }

        // configdomain

        inline Candidate ConfigDomain(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Targets(d, Find(d, 0xe, "488905 ? ? ? ? 488d0d ? ? ? ? 4883c4205b48ff25 ? ? ? 01"), 14, 10);
            Append(offsets, Targets(d, Find(d, 0xd, "83f06e69d093010001e8 ? ? 0400488d05 ? ? ? 01ba000100004c8d05 ? ? ? 01488905 ? ? ? 02488d0d ? ? ? 02"), 47, 43));
            Append(offsets, Targets(d, Find(d, 0xd, "83f16e69d193010001488d0d ? ? ? 02e8 ? ? 0400488d05 ? ? ? 01ba000100004c8d05 ? ? ? 01488905 ? ? ? 02488d0d ? ? ? 02"), 54, 50)); // 2.13
            Append(offsets, Targets(d, Find(d, 0x0, "4883ec28baa71ace87488d0d ? ? ? 02e8 ? ? 0200488d05 ? ? ? 01ba000100004c8d05 ? ? ? 01488905 ? ? ? 02488d0d ? ? ? 02"), 54, 50)); // 1.15
            if (offsets.size() != 1) return std::nullopt;
            Patch domain{ offsets[0], {}, {}, PatchKind::Data, true };
            return std::vector{ domain };
        }

        // protocol: "https://{0}" becomes "http://{0}", and the string's length, which the code
        // carries as an immediate, goes down by one with it.

        inline Candidate HttpsString(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Find(d, 0x8, "68747470733a2f2f7b307d00"); // "https://{0}"
            Append(offsets, Find(d, 0x8, "687474703a2f2f7b307d00")); // "http://{0}"
            Append(offsets, Find(d, 0x0, "68747470733a2f2f7b307d00"));
            Append(offsets, Find(d, 0x0, "687474703a2f2f7b307d00"));
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Patch{ offsets[0], Text("https://{0}"), Text("http://{0}"), PatchKind::Data, false } };
        }

        inline Candidate HttpsLengths1_16(std::span<const uint8_t> d)
        {
            const auto offsets = Find(d, 0xC, "c745B7 ? 000080488d05 ? ? ? 0148894dd7");
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0] + 3, "0B", "0A") };
        }

        inline Candidate HttpsLengthsPre3_30(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Find(d, 0xC, "488d55c7488d4d17e8 ? ? ? ff41b8");
            Append(offsets, Find(d, 0x5, "488d55c7488d4d17e8 ? ? ? ff41b8"));
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0] + 15, "0C", "0B") };
        }

        inline Candidate HttpsLengths3_30(std::span<const uint8_t> d)
        {
            const auto first = Find(d, 0x6, "c74547 ? 0000800fbae81f");
            const auto second = Find(d, 0x9, "85d2750cc703 ? 000080");
            if (first.size() != 1 || second.size() != 1) return std::nullopt;
            return std::vector{ Code(first[0] + 3, "0B", "0A"), Code(second[0] + 6, "0B", "0A") };
        }

        inline Candidate HttpsLength3_210(std::span<const uint8_t> d)
        {
            const auto offsets = Find(d, 0x7, "488945d741b8 ? 000080 448945b7");
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Code(offsets[0] + 6, "0B", "0A") };
        }

        inline Candidate Protocol(std::span<const uint8_t> d)
        {
            Candidate text = HttpsString(d);
            if (!text) return std::nullopt;
            std::vector<std::vector<Patch>> lengths;
            for (Candidate c : { HttpsLengths1_16(d), HttpsLengthsPre3_30(d), HttpsLengths3_30(d), HttpsLength3_210(d) })
                if (c) lengths.push_back(std::move(*c));
            if (lengths.size() != 1) return std::nullopt;
            std::vector<Patch> all = std::move(*text);
            all.insert(all.end(), lengths[0].begin(), lengths[0].end());
            return all;
        }

        // dynamic resources: flags the game reads, each a byte set to 1.

        inline Candidate DynresForceOffline(std::span<const uint8_t> d)
        {
            std::vector<uint32_t> offsets = Targets(d, Find(d, 0x4, "baa7935217488d0d"), 0x22, 0x1A); // 3.210
            Append(offsets, Targets(d, Find(d, 0x1, "83f17269c193010001488d0d ? ? ? 0383f06569d093010001e8 ? ? 0400488d05 ? ? ? 01c705 ? ? ? 0301000000"), 47, 39));
            Append(offsets, Targets(d, Find(d, 0xb, "83f17269c193010001488d0d ? ? ? 0283f06569d093010001e8 ? ? 0400488d05 ? ? ? 01c705 ? ? ? 0201000000"), 47, 39)); // 2.72
            Append(offsets, Targets(d, Find(d, 0x7, "83f17269c193010001488d0d ? ? ? 0283f06569d093010001e8 ? ? 0400488d05 ? ? ? 01c705 ? ? ? 0201000000"), 47, 39)); // 2.13
            Append(offsets, Targets(d, Find(d, 0x4, "ba2734ff12488d0d ? ? ? 02e8 ? ? 0200488d05 ? ? ? 01c705 ? ? ? 0201000000"), 34, 26)); // 1.15
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Data(offsets[0], "01", "00") };
        }

        inline Candidate DynresEnable(std::span<const uint8_t> d)
        {
            const auto offsets = Targets(d, Find(d, 0x4, "ba502e23f1488d0d"), 0x22, 0x1A); // 3.210
            if (offsets.size() != 1) return std::nullopt;
            return std::vector{ Data(offsets[0], "01", "00") };
        }

        /// The one candidate that matched, or nothing when none or several did.
        inline Candidate Only(std::initializer_list<Candidate> candidates)
        {
            Candidate only;
            int count = 0;
            for (const Candidate& c : candidates)
                if (c) { only = c; ++count; }
            return count == 1 ? only : std::nullopt;
        }
    }

    /// Scans `image` (the executable as mapped, indexed by RVA). Every family but dynresEnable must
    /// be found exactly once, as the patcher requires; otherwise `failure` names the first that was
    /// not, and the result is empty.
    inline std::optional<Patches> Scan(std::span<const uint8_t> image, ScanFailure& failure)
    {
        using namespace detail;
        failure = ScanFailure::None;
        Patches patches;

        const Candidate certpin = Only({ CertpinNearJump(image), CertpinNearJumpNew(image), CertpinShortJump(image) });
        if (!certpin) { failure = ScanFailure::CertPin; return std::nullopt; }
        const Candidate auth = Only({ AuthHeader3_210(image), AuthHeader3_30(image), AuthHeader2_72(image), AuthHeader1_15(image) });
        if (!auth) { failure = ScanFailure::AuthHeader; return std::nullopt; }
        const Candidate domain = ConfigDomain(image);
        if (!domain) { failure = ScanFailure::ConfigDomain; return std::nullopt; }
        const Candidate protocol = Protocol(image);
        if (!protocol) { failure = ScanFailure::Protocol; return std::nullopt; }
        const Candidate offline = DynresForceOffline(image);
        if (!offline) { failure = ScanFailure::DynresForceOffline; return std::nullopt; }

        patches.certpin = *certpin;
        patches.authheader = *auth;
        patches.configdomain = *domain;
        patches.protocol = *protocol;
        patches.dynresNoForceOffline = *offline;
        if (Candidate enable = DynresEnable(image)) patches.dynresEnable = std::move(*enable);
        return patches;
    }

    /// The config domain's value for a server on this machine: the patcher's default address, with
    /// the port only when it is not HTTP's own, since the game builds "http://{domain}/…" from it.
    inline size_t DomainFor(uint16_t port, char* output, size_t capacity)
    {
        const char* host = "127.0.0.1";
        char text[32];
        size_t length = 0;
        for (const char* c = host; *c; ++c) text[length++] = *c;
        if (port != 80)
        {
            text[length++] = ':';
            char digits[6];
            int count = 0;
            for (uint16_t p = port; p; p = static_cast<uint16_t>(p / 10)) digits[count++] = static_cast<char>('0' + p % 10);
            while (count) text[length++] = digits[--count];
        }
        if (length + 1 > capacity) return 0;
        std::memcpy(output, text, length);
        output[length] = 0;
        return length;
    }
}
