// QpdfOps.h — structural operations implemented with qpdf.
//
// qpdf handles whole-file transformations that PDFium does not do well:
// merge, split, encrypt/decrypt, linearize, repair. These functions do not
// touch PDFium and keep no global state, so they may run on any worker
// thread; each call owns its own QPDF objects. qpdf reports failures by
// throwing; every function here catches and converts to a Result so no
// exception crosses the PDFCore boundary.
#pragma once

#include "Result.h"

#include <string>
#include <vector>

namespace pdf {

// Number of pages according to qpdf (used to cross-check PDFium in tests).
Result<int> QpdfPageCount(const std::wstring& path);

// Concatenates all pages of `inputs` in order into `output` (atomic write).
// `firstPassword` opens inputs[0] (the document the user has open); the other
// inputs must open without a password. The output is not encrypted.
Status MergeFiles(const std::vector<std::wstring>& inputs, const std::wstring& output,
                  const std::string& firstPassword = {});

// Writes pages `pages` (0-based, in that order, duplicates allowed) of
// `input` into `output`. `password` opens an encrypted input; the output is
// not encrypted. Every write is atomic (temp file + rename).
Status ExtractPages(const std::wstring& input, const std::vector<int>& pages,
                    const std::wstring& output, const std::string& password = {});

// Splits `input` into one file per chunk; outputs[i] receives chunks[i].
Status SplitFile(const std::wstring& input, const std::vector<std::vector<int>>& chunks,
                 const std::vector<std::wstring>& outputs, const std::string& password = {});

// Encryption (AES-256, PDF 2.0 / R6). The owner password grants full access;
// the permissions apply to anyone opening with the user password. Passwords
// are UTF-8, never traced.
struct Permissions {
    bool print = true;
    bool copy = true;           // extract text and images
    bool modify = true;         // edit content
    bool annotate = true;       // comments and form fields
    bool assemble = true;       // insert, rotate, delete pages
};
Status EncryptPdf(const std::wstring& input, const std::wstring& output,
                   const std::string& userPassword, const std::string& ownerPassword,
                   const Permissions& perms, const std::string& inputPassword = {});

// Writes an unencrypted copy; needs the user or owner password.
Status DecryptPdf(const std::wstring& input, const std::wstring& output,
                   const std::string& password);

// Size optimisation: object streams + recompressed Flate, optional
// linearization ("fast web view"). Keeps the input's encryption.
struct CompressOptions {
    bool linearize = false;
};
Status CompressFile(const std::wstring& input, const std::wstring& output,
                    const CompressOptions& options, const std::string& password = {});

// Encryption state of a file, for the open-password prompt and the UI.
struct EncryptionInfo {
    bool encrypted = false;
    bool needsPassword = false;  // cannot be opened with an empty password
};
Result<EncryptionInfo> QueryEncryption(const std::wstring& input);

}  // namespace pdf
