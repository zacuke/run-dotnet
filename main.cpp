#include "util/https_download.h"
#include "util/extract_tar_gz.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;

// -------------------------------------------------------------------
// Logging helper (always flush)
// -------------------------------------------------------------------
static void log(const std::string& msg) {
    std::cerr << msg << std::endl;
    std::cerr.flush();
}

// -------------------------------------------------------------------
// Fork/exec wrapper
// -------------------------------------------------------------------
static bool run_process(const fs::path &exe, char *const argv[], const std::string &label) {
    log("Launching process: " + label + " [" + exe.string() + "]");
    pid_t pid = fork();
    if (pid == 0) {
        execv(exe.c_str(), argv);
        perror("[child] execv failed");
        _exit(127);
    }
    if (pid < 0) {
        perror("fork failed");
        return false;
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid failed");
        return false;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        log(label + " exited with " + std::to_string(code));
        return code == 0;
    }
    if (WIFSIGNALED(status)) {
        log(label + " killed by signal " + std::to_string(WTERMSIG(status)));
    }
    return false;
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main(int argc, char* argv[]) {
    try {
        log("dotnet bootstrapper started");

        fs::path projectRoot = fs::current_path();
        log("Project root: " + projectRoot.string());

        fs::path dotnetDir   = projectRoot / ".dotnet";
        fs::create_directories(dotnetDir);

        // Central store
        fs::path storeDir    = fs::path(getenv("HOME")) / ".local/share/run-dotnet";
        fs::path archivesDir = storeDir / "archives";
        fs::path versionsDir = storeDir / "versions";
        fs::create_directories(archivesDir);
        fs::create_directories(versionsDir);

        std::string pinnedVersion;
        int dotnetArgStart = 1;

        if (argc > 1) {
            std::string arg1 = argv[1];
            if (isdigit(arg1[0])) {
                pinnedVersion = arg1;
                dotnetArgStart = 2;
                log("Pinned version arg detected: " + pinnedVersion);
            }
        }

        // ---- Fetch releases-index.json
        log("Fetching releases-index.json ...");
        std::string jsonStr = https_get_string(
            "dotnetcli.blob.core.windows.net",
            "/dotnet/release-metadata/releases-index.json");
        log("Fetched releases-index.json, size=" + std::to_string(jsonStr.size()));

        std::stringstream ss(jsonStr);
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(ss, pt);

        std::string targetVersion;

        if (!pinnedVersion.empty()) {
            targetVersion = pinnedVersion;
            log("Using pinned .NET version: " + targetVersion);
        } else {
            log("Selecting latest LTS channel...");
            std::string releaseChannelUrl;
            int bestMajor = -1;

            for (auto &channel : pt.get_child("releases-index")) {
                try {
                    auto releaseTypeOpt = channel.second.get_optional<std::string>("release-type");
                    auto chanVerOpt     = channel.second.get_optional<std::string>("channel-version");
                    auto supportOpt     = channel.second.get_optional<std::string>("support-phase");

                    std::string url;
                    for (auto &field : channel.second) {
                        if (field.first == "releases.json") {
                            url = field.second.get_value<std::string>();
                            break;
                        }
                    }

                    if (releaseTypeOpt && *releaseTypeOpt == "lts" && !url.empty() && chanVerOpt) {
                        if (supportOpt && *supportOpt == "eol") continue; // skip dead LTS

                        int major = std::stoi(chanVerOpt->substr(0, chanVerOpt->find('.')));
                        if (major > bestMajor) {
                            bestMajor = major;
                            releaseChannelUrl = url;
                        }
                    }
                }
                catch (const std::exception &ex) {
                    log(std::string("Error parsing channel: ") + ex.what());
                }
            }

            if (releaseChannelUrl.empty()) {
                log("No active LTS found, trying STS...");
                for (auto &channel : pt.get_child("releases-index")) {
                    auto releaseTypeOpt = channel.second.get_optional<std::string>("release-type");
                    auto chanVerOpt     = channel.second.get_optional<std::string>("channel-version");

                    std::string url;
                    for (auto &field : channel.second) {
                        if (field.first == "releases.json") {
                            url = field.second.get_value<std::string>();
                            break;
                        }
                    }

                    if (releaseTypeOpt && *releaseTypeOpt == "sts" && !url.empty() && chanVerOpt) {
                        releaseChannelUrl = url;
                        log("Falling back to STS channel " + *chanVerOpt);
                        break;
                    }
                }
            }

            if (releaseChannelUrl.empty()) {
                log("Could not locate valid channel in releases-index.json");
                return 1;
            }
            log("Channel metadata URL: " + releaseChannelUrl);

            // Strip https://
            if (releaseChannelUrl.find("https://") == 0)
                releaseChannelUrl = releaseChannelUrl.substr(8);

            auto slashPos = releaseChannelUrl.find('/');
            std::string host = releaseChannelUrl.substr(0, slashPos);
            std::string path = releaseChannelUrl.substr(slashPos);

            log("Fetching channel JSON from " + host + path);
            std::string relJson = https_get_string(host, path);
            log("Fetched channel releases.json, size=" + std::to_string(relJson.size()));

            std::stringstream ss2(relJson);
            boost::property_tree::ptree pt2;
            boost::property_tree::read_json(ss2, pt2);

            targetVersion = pt2.get<std::string>("latest-release");
            log("Latest channel release: " + targetVersion);
        }

        std::string filename = "dotnet-sdk-" + targetVersion + "-linux-x64.tar.gz";
        std::string target   = "/dotnet/Sdk/" + targetVersion + "/" + filename;

        fs::path archivePath = archivesDir / filename;
        fs::path extractDir  = versionsDir / targetVersion;
        fs::path dotnetBin   = extractDir / "dotnet";

        if (!fs::exists(archivePath)) {
            log("Downloading archive from " + target);
            https_download("dotnetcli.azureedge.net", target, archivePath);
            log("Download complete: " + archivePath.string());
        }
        if (!fs::exists(dotnetBin)) {
            fs::create_directories(extractDir);
            log("Extracting archive...");
            if (!extract_tar_gz(archivePath.string(), extractDir.string())) {
                log("Extraction failed");
                return 1;
            }
            log("Extraction finished");
        }

        // Resync .dotnet symlinks
        log("Wiring .dotnet symlinks...");
        for (auto &e : fs::directory_iterator(dotnetDir)) {
            fs::remove_all(e.path());
        }
        for (auto &entry : fs::directory_iterator(extractDir)) {
            fs::path name = entry.path().filename();
            fs::path dest = dotnetDir / name;
            if (fs::exists(dest) || fs::is_symlink(dest)) fs::remove_all(dest);
            fs::create_symlink(entry.path(), dest);
        }

        fs::path projectDotnetBin = dotnetDir / "dotnet";
        if (!fs::exists(projectDotnetBin)) {
            log("dotnet binary not found in .dotnet");
            return 1;
        }

        // Restore if .csproj found
        fs::path csproj;
        for (auto &entry : fs::directory_iterator(projectRoot)) {
            if (entry.path().extension() == ".csproj") {
                csproj = entry.path();
                break;
            }
        }

        if (!csproj.empty()) {
            log("Found csproj: " + csproj.string());
            log("Running dotnet restore ...");
            char* restoreArgs[] = {
                const_cast<char*>(projectDotnetBin.c_str()),
                const_cast<char*>("restore"),
                const_cast<char*>(csproj.c_str()),
                nullptr
            };
            if (!run_process(projectDotnetBin, restoreArgs, "dotnet restore")) {
                log("dotnet restore failed");
                return 1;
            }
        }

        // If no args passed
        if (argc <= dotnetArgStart) {
            log(std::string("Usage: ") + argv[0] + " [X.Y.Z] <args to dotnet>");
            return 1;
        }

        // Run user dotnet command
        log("Invoking user command via .dotnet/dotnet");
        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(projectDotnetBin.c_str()));
        for (int i = dotnetArgStart; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        bool ok = run_process(projectDotnetBin, newArgs.data(), "dotnet main");
        return ok ? 0 : 1;
    }
    catch (const std::exception &e) {
        log(std::string("Error: ") + e.what());
        return 1;
    }
}