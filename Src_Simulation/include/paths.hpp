//=======================================================================
// Paths shown to the user are relative to the project root, never absolute: 
// a message reads "Src_Simulation/output/20260925_012005/plots/report_simu.html", not "/Users/<name>/.../Stock_Market_Trading_Simulator/Src_Simulation/output/..."
// The root is the closest parent folder holding both Src_Simulation/ and NRT/ (avoid having the full absolute path in logs, which is personal and doesn't matter to the user)
//=======================================================================
#ifndef PATHS_HPP
#define PATHS_HPP

#include <filesystem>
#include <string>

namespace sim {

inline std::string display_path(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec) {
        return path;
    }
    for (fs::path dir = absolute; !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_directory(dir / "Src_Simulation", ec) && fs::is_directory(dir / "NRT", ec)) {
            fs::path relative = fs::relative(absolute, dir, ec);
            return ec ? path : relative.string();
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    return path;
}

} // namespace sim

#endif // PATHS_HPP
