#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>

namespace opt {
// Staging belongs to one explicit export. It is private before any content is
// written, and replacement retains the destination's existing access controls.
class AtomicFile {
public:
    explicit AtomicFile(std::filesystem::path destination);
    ~AtomicFile();
    AtomicFile(const AtomicFile&)=delete;
    AtomicFile& operator=(const AtomicFile&)=delete;
    const std::filesystem::path& temporary_path()const;
    void write(const std::function<void(std::ostream&)>& writer);
    void publish(const std::function<void()>& before_replace={});
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
