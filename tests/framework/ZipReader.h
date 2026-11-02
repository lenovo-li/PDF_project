// ZipReader.h — reads back the archives ZipWriter produces, for tests.
//
// Walks the central directory, checks each local header against it, inflates
// method-8 entries with zlib and verifies the CRC. Only what ZipWriter emits
// is supported (no ZIP64, no data descriptors, no encryption); anything else
// makes Read return false, which a test reports as a failure.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pdftest {

struct ZipEntry {
    std::string data;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
};

inline std::uint32_t Rd16(const std::vector<std::uint8_t>& z, std::size_t at) {
    return z[at] | (z[at + 1] << 8);
}
inline std::uint32_t Rd32(const std::vector<std::uint8_t>& z, std::size_t at) {
    return Rd16(z, at) | (Rd16(z, at + 2) << 16);
}

// Entries by name. False on any structural error or CRC mismatch.
inline bool ReadZip(const std::vector<std::uint8_t>& z, std::map<std::string, ZipEntry>* out) {
    if (z.size() < 22) return false;
    const std::size_t eocd = z.size() - 22;  // ZipWriter writes no comment
    if (Rd32(z, eocd) != 0x06054b50) return false;
    const std::uint32_t count = Rd16(z, eocd + 10);
    std::size_t at = Rd32(z, eocd + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > z.size() || Rd32(z, at) != 0x02014b50) return false;
        ZipEntry e;
        e.flags = static_cast<std::uint16_t>(Rd16(z, at + 8));
        e.method = static_cast<std::uint16_t>(Rd16(z, at + 10));
        const std::uint32_t crc = Rd32(z, at + 16);
        const std::uint32_t csize = Rd32(z, at + 20), size = Rd32(z, at + 24);
        const std::uint32_t nlen = Rd16(z, at + 28);
        const std::uint32_t extra = Rd16(z, at + 30), comment = Rd16(z, at + 32);
        const std::size_t local = Rd32(z, at + 42);
        if (at + 46 + nlen > z.size()) return false;
        const std::string name(z.begin() + at + 46, z.begin() + at + 46 + nlen);
        at += 46 + nlen + extra + comment;

        if (local + 30 > z.size() || Rd32(z, local) != 0x04034b50) return false;
        if (Rd32(z, local + 14) != crc || Rd32(z, local + 18) != csize) return false;
        const std::size_t dataAt = local + 30 + Rd16(z, local + 26) + Rd16(z, local + 28);
        if (dataAt + csize > z.size()) return false;
        if (e.method == 0) {
            if (csize != size) return false;
            e.data.assign(z.begin() + dataAt, z.begin() + dataAt + size);
        } else if (e.method == 8) {
            e.data.resize(size);
            z_stream zs{};
            if (inflateInit2(&zs, -15) != Z_OK) return false;
            zs.next_in = const_cast<Bytef*>(z.data() + dataAt);
            zs.avail_in = csize;
            zs.next_out = reinterpret_cast<Bytef*>(e.data.data());
            zs.avail_out = size;
            const int rc = inflate(&zs, Z_FINISH);
            const uLong produced = zs.total_out;
            inflateEnd(&zs);
            if (rc != Z_STREAM_END || produced != size) return false;
        } else {
            return false;
        }
        const uLong got = crc32(crc32(0L, Z_NULL, 0),
                                reinterpret_cast<const Bytef*>(e.data.data()),
                                static_cast<uInt>(e.data.size()));
        if (got != crc) return false;
        (*out)[name] = std::move(e);
    }
    return true;
}

}  // namespace pdftest
