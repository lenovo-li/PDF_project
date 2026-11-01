// Result.h — value-or-error return type for the PDFCore public API.
//
// PDFCore never throws across its boundary: the UI layer runs its calls on a
// worker thread and marshals results back, and an exception escaping a worker
// would terminate the process. Errors carry a human-readable message (shown to
// the user or written to the trace) and, for PDFium failures, the
// FPDF_GetLastError() code so a bug report pinpoints the cause.
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace pdf {

struct Error {
    std::wstring message;
    unsigned long code = 0;  // FPDF_ERR_* for PDFium failures, 0 otherwise
};

template <typename T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error error) : v_(std::move(error)) {}

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() { return std::get<0>(v_); }
    const T& value() const { return std::get<0>(v_); }
    const Error& error() const { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

// Result<void> equivalent: success carries nothing.
struct Ok {};
using Status = Result<Ok>;

}  // namespace pdf
