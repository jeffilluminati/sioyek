#include <qfile.h>
#include <vector>

#include "checksum.h"

std::string compute_checksum(const QString& file_name, QCryptographicHash::Algorithm hash_algorithm)
{
    QFile infile(file_name);
    qint64 file_size = infile.size();
    // Large reads: with small ones, hashing a big document spends a lot of time in tens of thousands of
    // read() system calls and context switches. (We don't mmap the file because it may be truncated
    // while we read it, e.g. when LaTeX rewrites it, which would crash with SIGBUS.)
    const qint64 buffer_size = 1 << 20;

    if (infile.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
    {
        std::vector<char> buffer(qMin(qMax(file_size, qint64(1)), buffer_size));
        qint64 bytes_read;
        qint64 read_size = qMin(file_size, buffer_size);

        QCryptographicHash hash(hash_algorithm);
        while (read_size > 0 && (bytes_read = infile.read(buffer.data(), read_size)) > 0)
        {
            file_size -= bytes_read;
            hash.addData(buffer.data(), bytes_read);
            read_size = qMin(file_size, buffer_size);
        }

        infile.close();
        return QString(hash.result().toHex()).toStdString();
    }
    return "";
}

CachedChecksummer::CachedChecksummer(const std::vector<std::pair<std::wstring, std::wstring>>* loaded_checksums) {
    if (loaded_checksums) {
        for (const auto& [path, checksum_] : *loaded_checksums) {
            std::string checksum = QString::fromStdWString(checksum_).toStdString();
            cached_checksums[path] = checksum;
            cached_paths[checksum].push_back(path);
        }
    }
}

std::optional<std::string> CachedChecksummer::get_checksum_fast(std::wstring file_path) {
    // return the checksum only if it is alreay precomputed in cache
    if (cached_checksums.find(file_path) != cached_checksums.end()) {
        return cached_checksums[file_path];
    }
    return {};
}

std::string CachedChecksummer::get_checksum(std::wstring file_path) {

    auto cached_checksum = get_checksum_fast(file_path);

    if (!cached_checksum) {
        std::string checksum = compute_checksum(QString::fromStdWString(file_path), QCryptographicHash::Md5);
        cached_checksums[file_path] = checksum;
        cached_paths[checksum].push_back(file_path);
    }
    return cached_checksums[file_path];

}

std::optional<std::wstring> CachedChecksummer::get_path(std::string checksum) {
    const std::vector<std::wstring> paths = cached_paths[checksum];

    for (const auto& path_string : paths) {
        if (QFile::exists(QString::fromStdWString(path_string))) {
            return path_string;
        }
    }
    return {};
}

int CachedChecksummer::num_docs_with_checksum(std::string checksum) {
    if (cached_paths.find(checksum) != cached_paths.end()) {
        return cached_paths[checksum].size();
    }
    return 0;
}