#ifndef HTTPS_DOWNLOAD_H
#define HTTPS_DOWNLOAD_H

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

void https_download(const std::string& host, const std::string& target, const fs::path& outFile);
std::string https_get_string(const std::string& host, const std::string& target);

#endif // HTTPS_DOWNLOAD_H