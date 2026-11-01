// TestEngine.h — access to the process-wide engine thread from tests.
//
// Tests that touch PDFium wrap their body in pdftest::OnEngine([&] { ... }),
// which mirrors the production rule: every FPDF_* call runs on the engine
// thread. EXPECT_* macros are safe there because RunSync blocks the test
// thread until the job finishes.
#pragma once

#include <PDFCore/Engine.h>

#include <functional>

namespace pdftest {

pdf::Engine& SharedEngine();

inline void OnEngine(const std::function<void()>& body) { SharedEngine().RunSync(body); }

}  // namespace pdftest
