// Help's Copy Diagnostics text.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/diagnostics.h"
#include "core/log.h"

using namespace ustudio;
using namespace ustudio::app;

TEST_CASE("diagnostics: versions, sandbox, GPU, log folder and the last lines, nothing else")
{
    DiagnosticsFacts facts;
    facts.appVersion = "0.49.0-beta.2";
    facts.mltVersion = "7.40.0";
    facts.gtkVersion = "4.22.1";
    facts.adwaitaVersion = "1.9.2";
    facts.flatpak = true;
    facts.gpu = "On: Mesa Intel(R) Iris(R) Xe Graphics (ADL GT2)";
    facts.logFolder = "/home/u/.local/state/ustudio/logs";
    facts.recentLines = {"[a] one", "[b] two"};
    const std::string text = formatDiagnostics(facts);
    CHECK(text == "U-Stu Video Editor 0.49.0-beta.2\n"
                  "MLT 7.40.0, GTK 4.22.1, libadwaita 1.9.2\n"
                  "Flatpak: yes\n"
                  "GPU acceleration: On: Mesa Intel(R) Iris(R) Xe Graphics (ADL GT2)\n"
                  "Log folder: /home/u/.local/state/ustudio/logs\n"
                  "\n"
                  "Last 2 log lines:\n"
                  "[a] one\n"
                  "[b] two\n");
}

TEST_CASE("diagnostics: the log keeps its last lines, oldest first, at most 200")
{
    core::Log::setLevel(core::LogLevel::Debug);
    for (int i = 0; i < 250; ++i)
        core::Log::debug("[test] line " + std::to_string(i));
    const std::vector<std::string> last = core::Log::recentLines(50);
    REQUIRE(last.size() == 50);
    CHECK(last.front().ends_with("[test] line 200"));
    CHECK(last.back().ends_with("[test] line 249"));
    CHECK(core::Log::recentLines(1000).size() == 200);
    CHECK_FALSE(core::Log::directory().empty());
    CHECK(core::Log::currentFile().empty()); // init() wasn't called: no file to show
}
