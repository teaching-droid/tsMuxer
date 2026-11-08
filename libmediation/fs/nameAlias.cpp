// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "nameAlias.h"

#include <map>
#include <mutex>

namespace
{
// The readers open their files on threads of their own, so the table is read under a lock
std::mutex& lock()
{
    static std::mutex m;
    return m;
}
std::map<std::string, std::string>& table()
{
    static std::map<std::string, std::string> t;
    return t;
}
}  // namespace

namespace nameAlias
{
void add(const std::string& alias, const std::string& real)
{
    std::lock_guard<std::mutex> guard(lock());
    table()[alias] = real;
}

std::string resolve(const std::string& name)
{
    std::lock_guard<std::mutex> guard(lock());
    const auto it = table().find(name);
    return it == table().end() ? name : it->second;
}

std::string shown(std::string text)
{
    std::lock_guard<std::mutex> guard(lock());
    for (const auto& [alias, real] : table())
        for (size_t pos = text.find(alias); pos != std::string::npos; pos = text.find(alias, pos + real.size()))
            text.replace(pos, alias.size(), real);
    return text;
}
}  // namespace nameAlias
