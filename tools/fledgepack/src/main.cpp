// fledgepack - Fledge packager CLI.
//
//   fledgepack <project.fledge> [--out <dir>] [--runtime <fledge_runtime.exe>]
//            [--modules <dir>] [--sidecar] [--no-compress] [--no-icon] [--gui | --console]
//   fledgepack --cook <project.fledge> [--force]   (incremental cook into Intermediate/Cooked)
//   fledgepack --pak-only <project.fledge> <out.fpak>
//   fledgepack --list <file.fpak | Game.exe>
//   fledgepack --build-scripts <project.fledge> [--release | --debug] [--force] [--compiler g++.exe]
//   fledgepack --new-script <project.fledge> <ClassName> [behaviour|service|character|empty]
#include <cstdio>
#include <string>

#include "fledge/assets/pak.h"
#include "fledge/core/log.h"
#include "fledge/edition/edition.h"
#include "fledge/packager/cooker.h"
#include "fledge/packager/packager.h"
#include "fledge/packager/script_build.h"

using namespace fledge;

static int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  fledgepack <project.fledge> [--out dir] [--runtime exe] [--modules dir] [--sidecar] [--no-compress] [--no-icon] [--gui|--console]\n"
                 "  fledgepack --cook <project.fledge> [--force]\n"
                 "  fledgepack --pak-only <project.fledge> <out.fpak>\n"
                 "  fledgepack --list <file.fpak | Game.exe>\n"
                 "  fledgepack --build-scripts <project.fledge> [--release | --debug] [--force] [--compiler g++.exe]\n"
                 "  fledgepack --new-script <project.fledge> <ClassName> [behaviour|service|character|empty]\n");
    return 1;
}

int main(int argc, char** argv) {
    initEdition();
    if (argc < 2) return usage();
    std::string first = argv[1];
    if (first == "--list" && argc == 3) {
        fs::path p = pathFromUtf8(argv[2]);
        auto pak = PakReader::openAppended(p);
        if (!pak) pak = PakReader::openFile(p);
        if (!pak) { std::fprintf(stderr, "not a pak or packaged game: %s\n", argv[2]); return 2; }
        for (auto& [path, e] : pak->entries())
            std::printf("%-40s %10llu -> %10llu  %s  crc=%08x\n", path.c_str(), static_cast<unsigned long long>(e.originalSize),
                        static_cast<unsigned long long>(e.storedSize), e.compression == PakCompression::Lz ? "lz " : "raw", e.crc);
        return 0;
    }
    if (first == "--cook" && argc >= 3) {
        auto project = Project::load(pathFromUtf8(argv[2]));
        if (!project) {
            std::fprintf(stderr, "error: %s\n", project.error().c_str());
            return 2;
        }
        CookOptions opt;
        opt.force = argc > 3 && std::string(argv[3]) == "--force";
        auto r = cookProject(*project, opt, [](const std::string& s) { std::printf("  %s\n", s.c_str()); });
        if (!r) {
            std::fprintf(stderr, "cook error: %s\n", r.error().c_str());
            return 2;
        }
        std::printf("fledgepack: cook OK (%zu cooked, %zu up to date, %zu removed, %zu passthrough) -> %s\n", r->cooked, r->skipped,
                    r->removed, r->passthrough, narrow(project->cookDir().wstring()).c_str());
        return 0;
    }
    if (first == "--build-scripts" && argc >= 3) {
        auto project = Project::load(pathFromUtf8(argv[2]));
        if (!project) {
            std::fprintf(stderr, "error: %s\n", project.error().c_str());
            return 2;
        }
        ScriptBuildOptions opt;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--release") opt.release = true;
            else if (a == "--debug") opt.debug = true;  // -O0 -g for gdb sessions
            else if (a == "--force") opt.force = true;
            else if (a == "--compiler" && i + 1 < argc) {
                auto tc = findToolchain(pathFromUtf8(argv[++i]));
                if (!tc) {
                    std::fprintf(stderr, "error: %s\n", tc.error().c_str());
                    return 2;
                }
                opt.toolchain = *tc;
            } else return usage();
        }
        auto r = buildScripts(*project, opt, [](const std::string& s) { std::printf("  %s\n", s.c_str()); });
        for (auto& d : r.diagnostics)
            std::printf("%s:%d:%d: %s: %s\n", d.file.c_str(), d.line, d.column, toString(d.severity), d.message.c_str());
        std::printf("fledgepack: script build %s (%d errors, %d warnings, %d/%d compiled, %.2fs) -> %s\n", r.ok ? "OK" : "FAILED", r.errors(),
                    r.warnings(), r.compiled, r.sources + 1, r.seconds, narrow(r.module.wstring()).c_str());
        return r.ok ? 0 : 3;
    }
    if (first == "--new-script" && argc >= 4) {
        auto project = Project::load(pathFromUtf8(argv[2]));
        if (!project) {
            std::fprintf(stderr, "error: %s\n", project.error().c_str());
            return 2;
        }
        auto r = createScript(*project, argv[3], argc > 4 ? argv[4] : "behaviour");
        if (!r) {
            std::fprintf(stderr, "error: %s\n", r.error().c_str());
            return 2;
        }
        std::printf("created %s\n", narrow(r->wstring()).c_str());
        return 0;
    }
    if (first == "--pak-only" && argc == 4) {
        size_t n = 0;
        auto pak = buildProjectPak(pathFromUtf8(argv[2]), true, &n);
        if (!pak) { std::fprintf(stderr, "error: %s\n", pak.error().c_str()); return 2; }
        if (!writeFileBytes(pathFromUtf8(argv[3]), *pak)) return 2;
        std::printf("wrote %s (%zu files, %zu bytes)\n", argv[3], n, pak->size());
        return 0;
    }
    PackageOptions opt;
    opt.projectFile = pathFromUtf8(first);
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? pathFromUtf8(argv[++i]) : fs::path(); };
        if (a == "--out") opt.outputDir = next();
        else if (a == "--runtime") opt.runtimeExe = next();
        else if (a == "--modules") opt.moduleDir = next();
        else if (a == "--sidecar") opt.embedPak = false;
        else if (a == "--no-compress") opt.compress = false;
        else if (a == "--no-icon") opt.applyIcon = false;
        else if (a == "--gui") opt.subsystem = 2;
        else if (a == "--console") opt.subsystem = 3;
        else if (a == "--no-scripts") opt.buildScripts = false;
        else if (a == "--compiler") opt.compiler = next();
        else return usage();
    }
    auto r = packageProject(opt);
    if (!r) {
        std::fprintf(stderr, "fledgepack: error: %s\n", r.error().c_str());
        return 2;
    }
    std::printf("fledgepack: OK -> %s (%zu files, pak %llu bytes, modules %zu, icon %s, subsystem %s, scripts %s)\n",
                narrow(r->executable.wstring()).c_str(), r->fileCount, static_cast<unsigned long long>(r->pakBytes),
                r->copiedModules.size(), r->iconApplied ? "yes" : "no", r->subsystem == 2 ? "gui" : "console",
                r->scriptModule.empty() ? "none" : r->scriptModule.c_str());
    return 0;
}
