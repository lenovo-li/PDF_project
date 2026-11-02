#include "ZipWriter.h"

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace pdf {

namespace {
constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();

void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// Fixed DOS date/time (1980-01-01 00:00): the archive's content is what
// matters, and a constant keeps the output byte-for-byte reproducible.
constexpr std::uint16_t kDosTime = 0;
constexpr std::uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;
constexpr std::uint16_t kFlagUtf8 = 1u << 11;
constexpr std::uint16_t kVersion = 20;  // 2.0: deflate

// Raw deflate (no zlib header), as zip method 8 stores it.
Result<std::vector<std::uint8_t>> Deflate(const std::uint8_t* data, std::size_t size) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return Error{L"压缩初始化失败"};
    std::vector<std::uint8_t> out(deflateBound(&zs, static_cast<uLong>(size)));
    zs.next_in = const_cast<Bytef*>(data);
    zs.avail_in = static_cast<uInt>(size);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const std::size_t written = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return Error{L"压缩失败"};
    out.resize(written);
    return out;
}
}  // namespace

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    // zlib's crc32 takes uInt lengths; feed large buffers in pieces.
    uLong crc = crc32(0L, Z_NULL, 0);
    while (size > 0) {
        const uInt n = static_cast<uInt>(std::min<std::size_t>(size, 1u << 30));
        crc = crc32(crc, data, n);
        data += n;
        size -= n;
    }
    return static_cast<std::uint32_t>(crc);
}

Status ZipWriter::Add(const std::string& name, const std::string& text, bool compress) {
    return Add(name, std::vector<std::uint8_t>(text.begin(), text.end()), compress);
}

Status ZipWriter::Add(const std::string& name, const std::vector<std::uint8_t>& data,
                      bool compress) {
    if (name.empty() || name.size() > 0xFFFF || name.front() == '/')
        return Error{L"压缩包条目名无效"};
    for (const Entry& e : entries_)
        if (e.name == name) return Error{L"压缩包条目重复"};
    if (entries_.size() >= 0xFFFF || data.size() > kMax32) return Error{L"压缩包过大"};

    Entry e;
    e.name = name;
    e.size = static_cast<std::uint32_t>(data.size());
    e.crc = Crc32(data.data(), data.size());
    std::vector<std::uint8_t> deflated;
    const std::vector<std::uint8_t>* body = &data;
    if (compress && !data.empty()) {
        auto d = Deflate(data.data(), data.size());
        if (!d) return d.error();
        // Keep the smaller form: tiny or incompressible entries are stored.
        if (d.value().size() < data.size()) {
            deflated = std::move(d.value());
            body = &deflated;
            e.method = 8;
        }
    }
    e.compressedSize = static_cast<std::uint32_t>(body->size());
    if (out_.size() + 30 + name.size() + body->size() > kMax32) return Error{L"压缩包过大"};
    e.localOffset = static_cast<std::uint32_t>(out_.size());

    Put32(out_, 0x04034b50);  // local file header
    Put16(out_, kVersion);
    Put16(out_, kFlagUtf8);
    Put16(out_, e.method);
    Put16(out_, kDosTime);
    Put16(out_, kDosDate);
    Put32(out_, e.crc);
    Put32(out_, e.compressedSize);
    Put32(out_, e.size);
    Put16(out_, static_cast<std::uint16_t>(name.size()));
    Put16(out_, 0);  // extra field length
    out_.insert(out_.end(), name.begin(), name.end());
    out_.insert(out_.end(), body->begin(), body->end());
    entries_.push_back(std::move(e));
    return Ok{};
}

std::vector<std::uint8_t> ZipWriter::Finish() {
    const std::uint32_t cdOffset = static_cast<std::uint32_t>(out_.size());
    for (const Entry& e : entries_) {
        Put32(out_, 0x02014b50);  // central directory header
        Put16(out_, kVersion);    // version made by (0 = MS-DOS attribute host)
        Put16(out_, kVersion);    // version needed
        Put16(out_, kFlagUtf8);
        Put16(out_, e.method);
        Put16(out_, kDosTime);
        Put16(out_, kDosDate);
        Put32(out_, e.crc);
        Put32(out_, e.compressedSize);
        Put32(out_, e.size);
        Put16(out_, static_cast<std::uint16_t>(e.name.size()));
        Put16(out_, 0);  // extra
        Put16(out_, 0);  // comment
        Put16(out_, 0);  // disk number
        Put16(out_, 0);  // internal attributes
        Put32(out_, 0);  // external attributes
        Put32(out_, e.localOffset);
        out_.insert(out_.end(), e.name.begin(), e.name.end());
    }
    const std::uint32_t cdSize = static_cast<std::uint32_t>(out_.size()) - cdOffset;
    Put32(out_, 0x06054b50);  // end of central directory
    Put16(out_, 0);
    Put16(out_, 0);
    Put16(out_, static_cast<std::uint16_t>(entries_.size()));
    Put16(out_, static_cast<std::uint16_t>(entries_.size()));
    Put32(out_, cdSize);
    Put32(out_, cdOffset);
    Put16(out_, 0);  // comment length
    std::vector<std::uint8_t> result = std::move(out_);
    out_.clear();
    entries_.clear();
    return result;
}

}  // namespace pdf
