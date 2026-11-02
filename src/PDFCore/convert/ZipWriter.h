// ZipWriter.h — minimal in-memory zip archive writer (for .docx packages).
//
// Writes a standard PKZIP archive: deflate-compressed entries (zlib, raw
// deflate), CRC-32, UTF-8 names (general-purpose flag bit 11), sizes known
// up front so no data descriptors. No ZIP64: each entry and the whole archive
// must stay under 4 GiB and 65535 entries; Add refuses anything larger, which
// a converted document never approaches.
//
// Pure: no PDFium, no file I/O. The caller writes Finish()'s bytes with
// WriteFileAtomic.
#pragma once

#include "../Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

class ZipWriter {
public:
    // Adds an entry. `name` is the path inside the archive, '/'-separated,
    // UTF-8 (e.g. "word/document.xml"). `compress` false stores the bytes
    // as-is (for already-compressed data such as PNG). Names must be unique.
    Status Add(const std::string& name, const std::vector<std::uint8_t>& data,
               bool compress = true);
    Status Add(const std::string& name, const std::string& text, bool compress = true);

    // Appends the central directory and returns the archive. The writer is
    // left empty and can be reused.
    std::vector<std::uint8_t> Finish();

    std::size_t EntryCount() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t compressedSize = 0;
        std::uint32_t size = 0;
        std::uint16_t method = 0;  // 0 stored, 8 deflate
        std::uint32_t localOffset = 0;
    };
    std::vector<Entry> entries_;
    std::vector<std::uint8_t> out_;
};

// CRC-32 (IEEE 802.3, as zip uses), exposed for tests.
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size);

}  // namespace pdf
