// SPDX-License-Identifier: GPL-3.0-or-later
// doctest main, plus two things for unattended runs (CI):
//  - BAM_TEST_PROGRESS=1 prints every test case (and subcase) when it starts and ends, with its
//    duration, to stderr right away, so a run that hangs names the test it hangs in;
//  - on Windows no error dialog may wait for a click (abort(), a crash, a missing DLL): the
//    process ends with an error code instead.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace {

bool progress_enabled() {
  const char* v = std::getenv("BAM_TEST_PROGRESS");
  return v && *v && std::string(v) != "0";
}

struct ProgressListener : doctest::IReporter {
  using Clock = std::chrono::steady_clock;
  bool on = progress_enabled();
  Clock::time_point started{};
  int depth = 0;

  explicit ProgressListener(const doctest::ContextOptions&) {}

  void report_query(const doctest::QueryData&) override {}
  void test_run_start() override {}
  void test_run_end(const doctest::TestRunStats&) override {}
  void test_case_start(const doctest::TestCaseData& tc) override {
    if (!on) return;
    started = Clock::now();
    std::fprintf(stderr, "[test] start: %s\n", tc.m_name);
    std::fflush(stderr);
  }
  void test_case_reenter(const doctest::TestCaseData&) override {}
  void test_case_end(const doctest::CurrentTestCaseStats& st) override {
    if (!on) return;
    const double s = std::chrono::duration<double>(Clock::now() - started).count();
    std::fprintf(stderr, "[test] end (%.2f s)%s\n", s, st.testCaseSuccess ? "" : " FAILED");
    std::fflush(stderr);
  }
  void test_case_exception(const doctest::TestCaseException& e) override {
    if (!on) return;
    std::fprintf(stderr, "[test] exception: %s\n", e.error_string.c_str());
    std::fflush(stderr);
  }
  void subcase_start(const doctest::SubcaseSignature& sc) override {
    if (!on) return;
    std::fprintf(stderr, "[test]   subcase: %s\n", sc.m_name.c_str());
    std::fflush(stderr);
  }
  void subcase_end() override {}
  void log_assert(const doctest::AssertData&) override {}
  void log_message(const doctest::MessageData&) override {}
  void test_case_skipped(const doctest::TestCaseData&) override {}
};

#ifdef _WIN32
void no_error_dialogs() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // abort(): no message box, no WER
#ifdef _DEBUG  // the debug CRT reports to stderr instead of a dialog (release builds have no such reports)
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
}
#endif

}  // namespace

DOCTEST_REGISTER_LISTENER("bam_progress", 1, ProgressListener);

int main(int argc, char** argv) {
#ifdef _WIN32
  no_error_dialogs();
#endif
  // an exception that escapes a helper thread (e.g. a failed REQUIRE there) ends the run with
  // a message instead of a silent abort
  std::set_terminate([] {
    std::fprintf(stderr, "[test] std::terminate called (an exception escaped a thread?)\n");
    std::fflush(stderr);
    std::_Exit(3);
  });
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
