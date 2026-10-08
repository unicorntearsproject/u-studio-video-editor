#include "diagnostics.h"

namespace ustudio::app {

std::string formatDiagnostics(const DiagnosticsFacts &facts)
{
    std::string text = "U-Stu Video Editor " + facts.appVersion + "\n";
    text += "MLT " + facts.mltVersion + ", GTK " + facts.gtkVersion + ", libadwaita " + facts.adwaitaVersion + "\n";
    text += std::string("Flatpak: ") + (facts.flatpak ? "yes" : "no") + "\n";
    text += "GPU acceleration: " + facts.gpu + "\n";
    text += "Log folder: " + facts.logFolder + "\n";
    text += "\nLast " + std::to_string(facts.recentLines.size()) + " log lines:\n";
    for (const std::string &line : facts.recentLines)
        text += line + "\n";
    return text;
}

} // namespace ustudio::app
