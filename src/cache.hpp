#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zerohttpd {
struct FileVersion {
    std::uint64_t inode = 0, size = 0;
    std::int64_t mtime_sec = 0, ctime_sec = 0;
    std::uint32_t mtime_ns = 0, ctime_ns = 0, dev_major = 0, dev_minor = 0;
    bool operator==(const FileVersion& v) const {
        return inode == v.inode && size == v.size && mtime_sec == v.mtime_sec &&
            ctime_sec == v.ctime_sec && mtime_ns == v.mtime_ns && ctime_ns == v.ctime_ns &&
            dev_major == v.dev_major && dev_minor == v.dev_minor;
    }
};
using Bytes = std::vector<char>;
class FileCache {
    struct Entry { FileVersion version; std::shared_ptr<const Bytes> data; std::list<std::string>::iterator lru; };
    std::size_t capacity_, used_ = 0;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> entries_;
    void erase(std::unordered_map<std::string, Entry>::iterator it) {
        used_ -= it->second.data->size(); order_.erase(it->second.lru); entries_.erase(it);
    }
public:
    explicit FileCache(std::size_t capacity) : capacity_(capacity) {}
    std::shared_ptr<const Bytes> get(const std::string& key, const FileVersion& version) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return {};
        if (!(it->second.version == version)) { erase(it); return {}; }
        order_.splice(order_.begin(), order_, it->second.lru);
        return it->second.data;
    }
    void put(const std::string& key, const FileVersion& version, std::shared_ptr<const Bytes> data) {
        if (capacity_ == 0 || data->empty() || data->size() > capacity_) return;
        auto existing = entries_.find(key);
        if (existing != entries_.end()) erase(existing);
        while (!order_.empty() && used_ + data->size() > capacity_) erase(entries_.find(order_.back()));
        order_.push_front(key);
        used_ += data->size();
        entries_.emplace(key, Entry{version, std::move(data), order_.begin()});
    }
    std::size_t used() const { return used_; }
    std::size_t entries() const { return entries_.size(); }
};
}
