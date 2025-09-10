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
    //log("Launching process: " + label + " [" + exe.string() + "]");
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

        //log("Channel candidate: " + ver + " " + type + " " + phase);
        if (type == "lts" && phase == "active" && !ver.empty() && !url.empty()) {
            int major = std::stoi(ver.substr(0, ver.find('.')));
            if (major > bestMajor) {
                bestMajor = major;
                result = url;
            }
        }
    }

    if (!result.empty()) {
       // log("Selected active LTS channel: " + result);
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
static std::string pick_asset_url(const json& channel,
                                  const std::string& targetVersion,
                                  const std::string& rid = "linux-x64") {
    auto pickFile = [&](const json& files, const std::string& label) -> std::string {
        for (auto& f : files) {
            std::string fRid  = f.value("rid", "");
            std::string fUrl  = f.value("url", "");
            std::string fName = f.value("name", "");
            std::string fType = f.value("file-type", "");
            // prefer .tar.gz that matches rid
            if (fRid == rid && !fUrl.empty()) {
                if (fType == "installer" || fName.find(".tar.gz") != std::string::npos) {
                    log("Selected " + label + " asset: " + fName);
                    return fUrl;
                }
            }
        }
        return {};
    };

    for (auto& release : channel["releases"]) {
        std::string ver = release.value("release-version", "");
        if (ver != targetVersion) continue;

        // Prefer SDK
        if (release.contains("sdk") && release["sdk"].contains("files")) {
            std::string url = pickFile(release["sdk"]["files"], "SDK");
            if (!url.empty()) return url;
        }

        // Otherwise try runtime
        if (release.contains("runtime") && release["runtime"].contains("files")) {
            std::string url = pickFile(release["runtime"]["files"], "runtime");
            if (!url.empty()) return url;
        }
    }

    log("pick_asset_url: No matching asset found for version " + targetVersion + " and rid " + rid);
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
 

        // ---- Pick asset URL
        std::string downloadUrl;
 
        if (!pinnedVersion.empty()) {
            // ---- Figure out major from pinned (e.g. "10" -> "10")
            std::string majorStr = pinnedVersion.substr(0, pinnedVersion.find('.'));

            // Find matching channel in releases-index
            std::string pinnedChannelUrl;
            for (auto& entry : index["releases-index"]) {
                std::string chanVer = entry.value("channel-version", "");
                if (chanVer.rfind(majorStr, 0) == 0) { // starts with major
                    pinnedChannelUrl = entry.value("releases.json", "");
                    break;
                }
            }

            if (pinnedChannelUrl.empty()) {
                log("No channel found for major " + majorStr);
                return 1;
            }

            std::string host, path;
            split_url(pinnedChannelUrl, host, path);
            std::string channelStr = https_get_string(host, path);
            channelJson = json::parse(channelStr);

            // Pick target version:
            // If pinned is just "10" → resolve to channelJson["latest-release"]
            if (pinnedVersion.find('.') == std::string::npos) {
                pinnedVersion = channelJson.value("latest-release", "");
                log("Resolved major " + majorStr + " to latest " + pinnedVersion);
            }
            // If pinned is "10.0" → find latest patch in that band
            else if (std::count(pinnedVersion.begin(), pinnedVersion.end(), '.') == 1) {
                std::string best;
                for (auto& release : channelJson["releases"]) {
                    std::string relVer = release.value("release-version", "");
                    if (relVer.rfind(pinnedVersion + ".", 0) == 0) {
                        if (best.empty() || relVer > best) best = relVer;
                    }
                }
                if (!best.empty()) {
                    pinnedVersion = best;
                    log("Resolved " + pinnedVersion + ".* to " + best);
                }
            }
            // else exact match assumed

            downloadUrl = pick_asset_url(channelJson, pinnedVersion);
            if (downloadUrl.empty()) {
                log("No asset found for pinned " + pinnedVersion);
                return 1;
            }
        } else {
            // Original non-pinned path
            std::string host, path;
            split_url(channelUrl, host, path);
            std::string channelStr = https_get_string(host, path);
            channelJson = json::parse(channelStr);
            targetVersion = channelJson.value("latest-release", "");
            downloadUrl = pick_asset_url(channelJson, targetVersion);
            if (downloadUrl.empty()) {
                log("No asset found for " + targetVersion);
                return 1;
            }
        }

       // log("Download URL: " + downloadUrl);
        std::string host, path;
        split_url(downloadUrl, host, path);

        fs::path archivePath = archivesDir / fs::path(path).filename();
        fs::path extractDir  = versionsDir / targetVersion;
        fs::path dotnetBin   = extractDir / "dotnet";

        if (!fs::exists(archivePath)) {
            //log("Downloading archive...");
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