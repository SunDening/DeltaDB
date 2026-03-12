#include <fstream>
#include <filesystem>

#include "wal_manifest.h"

WalManifest::WalManifest(const std::string& manifest_dir)
    : manifest_dir_(manifest_dir) {
        manifest_path_ = manifest_dir + "manifest.txt";
    }

bool WalManifest::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_wals_.clear();

    std::ifstream in(manifest_path_);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            pending_wals_.insert(line);
    }

    return true;
}

std::string WalManifest::get_next_wal_name() {
    auto now = std::chrono::system_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()
		).count();
	return "wal_" + std::to_string(ms) + ".log";
}

void WalManifest::add_pending_wal(const std::string& wal_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_wals_.insert(wal_name);
    rewrite_manifest();
}

void WalManifest::remove_wal(const std::string& wal_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    pending_wals_.erase(wal_name);
    rewrite_manifest();

    std::filesystem::path wal_path = std::filesystem::path(manifest_dir_) / wal_name;
    std::error_code ec;
    std::filesystem::remove(wal_path, ec);
    if (ec) {
        std::cerr << "[Manifest] Failed to delete WAL file: " << wal_path << " - " << ec.message() << std::endl;
    }
}

std::vector<std::string> WalManifest::get_pending_wals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> pending_wal_vec = std::vector<std::string>(pending_wals_.begin(), pending_wals_.end());
    sort(pending_wal_vec.begin(), pending_wal_vec.end());
    return pending_wal_vec;
}

bool WalManifest::rewrite_manifest() {
    std::string tmp_path = manifest_dir_ + "manifest.tmp";

    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) return false;

        for (const auto& name : pending_wals_) {
            out << name << "\n";
        }

        out.flush();
        out.close();
    }

    // Ô­×ÓÌæ»»
    std::error_code ec;
    std::filesystem::rename(tmp_path, manifest_path_, ec);
    return !ec;
}
