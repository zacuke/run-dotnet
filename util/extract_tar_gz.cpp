#include "extract_tar_gz.h"
#include <archive.h>
#include <archive_entry.h>
#include <iostream>
#include <string>

bool extract_tar_gz(const std::string& archivePath, const std::string& destDir) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int r;

    a = archive_read_new();
    archive_read_support_filter_gzip(a);   // 🔹 support .gz compression
    archive_read_support_format_tar(a);

    if ((r = archive_read_open_filename(a, archivePath.c_str(), 10240)) != ARCHIVE_OK) {
        std::cerr << "archive_read_open_filename failed: "
                  << archive_error_string(a) << "\n";
        return false;
    }

    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    while (true) {
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK)
            std::cerr << "Warning: " << archive_error_string(a) << "\n";
        if (r < ARCHIVE_WARN) {
            archive_read_close(a);
            archive_read_free(a);
            archive_write_close(ext);
            archive_write_free(ext);
            return false;
        }

        // mimic `--strip-components=1`
        std::string path = archive_entry_pathname(entry);
        auto slashPos = path.find('/');
        if (slashPos != std::string::npos)
            path = path.substr(slashPos + 1);
        if (path.empty()) continue;

        std::string fullOutputPath = destDir + "/" + path;
        archive_entry_set_pathname(entry, fullOutputPath.c_str());

        r = archive_write_header(ext, entry);
        if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
            const void *buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                r = archive_read_data_block(a, &buff, &size, &offset);
                if (r == ARCHIVE_EOF) break;
                if (r < ARCHIVE_OK) break;
                r = archive_write_data_block(ext, buff, size, offset);
                if (r < ARCHIVE_OK) break;
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    return true;
}