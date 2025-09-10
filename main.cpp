#include "util/https_download.h"
#include "util/extract_tar_gz.h"
#include <nlohmann/json.hpp>
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
using json = nlohmann::json;

// -------------------------------------------------------------------
// Logging
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
// Split https://host/path into host+path
// -------------------------------------------------------------------
static void split_url(const std::string& fullUrl, std::string& host, std::string& path) {
    if (fullUrl.find("https://") == 0) {
        auto noScheme = fullUrl.substr(8);
        auto slashPos = noScheme.find('/');
        host = noScheme.substr(0, slashPos);
        path = noScheme.substr(slashPos);
    } else {
        throw std::runtime_error("Unsupported URL: " + fullUrl);
    }
}

// -------------------------------------------------------------------
// Pick channel URL (prefer LTS + active, fallback STS)
// -------------------------------------------------------------------
static std::string pick_channel_url(const json& index) {
    std::string result;
    int bestMajor = -1;

    for (auto& entry : index["releases-index"]) {
        std::string type  = entry.value("release-type", "");
        std::string phase = entry.value("support-phase", "");
        std::string ver   = entry.value("channel-version", "");
        std::string url   = entry.value("releases.json", "");

        log("Channel candidate: " + ver + " " + type + " " + phase);
        if (type == "lts" && phase == "active" && !ver.empty() && !url.empty()) {
            int major = std::stoi(ver.substr(0, ver.find('.')));
            if (major > bestMajor) {
                bestMajor = major;
                result = url;
            }
        }
    }

    if (!result.empty()) {
        log("Selected active LTS channel: " + result);
        return result;
    }

    log("No active LTS found, trying STS...");
    for (auto& entry : index["releases-index"]) {
        std::string type = entry.value("release-type", "");
        std::string ver  = entry.value("channel-version", "");
        std::string url  = entry.value("releases.json", "");
        if (type == "sts" && !url.empty()) {
            log("Fallback to STS " + ver);
            return url;
        }
    }

    return {};
}

// -------------------------------------------------------------------
// Pick SDK/runtime asset (prefer SDK for linux-x64)
// -------------------------------------------------------------------
static std::string pick_asset_url(const json& channel, const std::string& targetVersion,
                                  const std::string& rid = "linux-x64") {
    for (auto& release : channel["releases"]) {
        std::string ver = release.value("release-version", "");
        if (ver == targetVersion) {
            if (release.contains("sdk") && release["sdk"].contains("files")) {
                for (auto& f : release["sdk"]["files"]) {
                    if (f.value("rid", "") == rid)
                        return f.value("url", "");
                }
            }
            if (release.contains("runtime") && release["runtime"].contains("files")) {
                for (auto& f : release["runtime"]["files"]) {
                    if (f.value("rid", "") == rid)
                        return f.value("url", "");
                }
            }
        }
    }
    return {};
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main(int argc, char* argv[]) {
    try {
        log("dotnet bootstrapper started");

        fs::path projectRoot = fs::current_path();
        fs::path dotnetDir   = projectRoot / ".dotnet";
        fs::create_directories(dotnetDir);

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
                log("Pinned version: " + pinnedVersion);
            }
        }

        // ---- Fetch release index
        std::string idxStr = https_get_string(
            "dotnetcli.blob.core.windows.net",
            "/dotnet/release-metadata/releases-index.json");
        json index = json::parse(idxStr);

        std::string channelUrl;
        if (pinnedVersion.empty()) {
            channelUrl = pick_channel_url(index);
            if (channelUrl.empty()) {
                log("Could not determine channel URL");
                return 1;
            }
        }

        // ---- Fetch channel JSON
        std::string targetVersion = pinnedVersion;
        json channelJson;
        if (pinnedVersion.empty()) {
            std::string host, path;
            split_url(channelUrl, host, path);
            std::string channelStr = https_get_string(host, path);
            channelJson = json::parse(channelStr);
            targetVersion = channelJson.value("latest-release", "");
            log("Latest release in channel: " + targetVersion);
        }

        // ---- Pick asset URL
        std::string downloadUrl;
        if (!pinnedVersion.empty()) {
            log("Pinned version only provided — downloads based on pinned URL unsupported in this demo");
            return 1;
        } else {
            downloadUrl = pick_asset_url(channelJson, targetVersion);
            if (downloadUrl.empty()) {
                log("No asset found for " + targetVersion);
                return 1;
            }
        }

        log("Download URL: " + downloadUrl);
        std::string host, path;
        split_url(downloadUrl, host, path);

        fs::path archivePath = archivesDir / fs::path(path).filename();
        fs::path extractDir  = versionsDir / targetVersion;
        fs::path dotnetBin   = extractDir / "dotnet";

        if (!fs::exists(archivePath)) {
            log("Downloading archive...");
            https_download(host, path, archivePath);
        }
        if (!fs::exists(dotnetBin)) {
            fs::create_directories(extractDir);
            if (!extract_tar_gz(archivePath.string(), extractDir.string())) {
                log("Extraction failed");
                return 1;
            }
        }

        // ---- Update symlinks
        for (auto &e : fs::directory_iterator(dotnetDir))
            fs::remove_all(e.path());
        for (auto &entry : fs::directory_iterator(extractDir))
            fs::create_symlink(entry.path(), dotnetDir / entry.path().filename());

        fs::path projectDotnetBin = dotnetDir / "dotnet";
        if (!fs::exists(projectDotnetBin)) {
            log("dotnet binary not found after extraction");
            return 1;
        }

        // ---- Run dotnet restore if csproj found
        fs::path csproj;
        for (auto &entry : fs::directory_iterator(projectRoot)) {
            if (entry.path().extension() == ".csproj") {
                csproj = entry.path();
                break;
            }
        }
        if (!csproj.empty()) {
            char* restoreArgs[] = {
                const_cast<char*>(projectDotnetBin.c_str()),
                const_cast<char*>("restore"),
                const_cast<char*>(csproj.c_str()),
                nullptr
            };
            if (!run_process(projectDotnetBin, restoreArgs, "dotnet restore")) {
                return 1;
            }
        }

        // ---- Run user arguments
        if (argc <= dotnetArgStart) {
            log(std::string("Usage: ") + argv[0] + " [X.Y.Z] <args to dotnet>");
            return 1;
        }

        std::vector<char*> newArgs;
        newArgs.push_back(const_cast<char*>(projectDotnetBin.c_str()));
        for (int i = dotnetArgStart; i < argc; i++)
            newArgs.push_back(argv[i]);
        newArgs.push_back(nullptr);

        return run_process(projectDotnetBin, newArgs.data(), "dotnet main") ? 0 : 1;
    }
    catch (const std::exception &e) {
        log(std::string("Error: ") + e.what());
        return 1;
    }
}