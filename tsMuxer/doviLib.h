#ifndef DOVI_LIB_H_
#define DOVI_LIB_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Optional runtime binding to libdovi, the library behind the usual Dolby Vision tooling, used to
// convert a profile 7 RPU to profile 8.1.
//
// Loaded BY NAME at run time rather than linked, for three reasons: the library is not needed for
// anything else tsMuxeR does, a prebuilt binary is published for one platform only, and its absence
// has to mean "this one feature is unavailable" rather than "tsMuxeR does not start". Nothing here
// is required to build or run tsMuxeR.
//
// Only six entry points are used, so they are declared here rather than pulling in libdovi's own
// header: that keeps a third-party header out of the tree and means no import library is needed.
// The signatures were checked against libdovi 3.4.0 by calling them.
//
// The types are opaque on purpose. DoviData is the one structure that has to be described, because
// its two fields are read directly; it is a pointer and a length and has been stable across
// releases.
class DoviLib
{
   public:
    // Loads the library on first use. Safe to call repeatedly; a failed load is not retried.
    static DoviLib& instance();

    [[nodiscard]] bool available() const { return m_ok; }

    // Why the library is unavailable, for a message that tells the user what to do about it.
    [[nodiscard]] const std::string& loadError() const { return m_loadError; }

    // True when the file is there but could not be used, so a message does not send the user to
    // fetch a file they already have.
    [[nodiscard]] bool fileFound() const { return m_fileFound; }

    // The name looked for, so a message can say exactly what is missing.
    static const char* libraryName();

    // Convert one profile 7 RPU NAL to profile 8.1. The input is the complete unspecified-62 NAL
    // starting at its two byte NAL header; the output is the same shape, ready to be written back
    // in place of the original. Returns false and fills err on any failure, leaving out untouched.
    //
    // The caller keeps the original bytes: nothing here writes over the input, because the original
    // RPU is what makes the conversion reversible.
    bool convertRpuToProfile81(const uint8_t* nal, size_t len, std::vector<uint8_t>& out, std::string& err);

   private:
    DoviLib();
    ~DoviLib();
    DoviLib(const DoviLib&) = delete;
    DoviLib& operator=(const DoviLib&) = delete;

    struct DoviData
    {
        const uint8_t* data;
        size_t len;
    };

    using ParseUnspec62Fn = void* (*)(const uint8_t*, size_t);
    using GetErrorFn = const char* (*)(void*);
    using ConvertWithModeFn = int32_t (*)(void*, uint8_t);
    using WriteUnspec62Fn = const DoviData* (*)(void*);
    using DataFreeFn = void (*)(const DoviData*);
    using RpuFreeFn = void (*)(void*);

    void* m_handle = nullptr;
    bool m_ok = false;
    bool m_fileFound = false;
    std::string m_loadError;

    ParseUnspec62Fn m_parse = nullptr;
    GetErrorFn m_getError = nullptr;
    ConvertWithModeFn m_convert = nullptr;
    WriteUnspec62Fn m_write = nullptr;
    DataFreeFn m_dataFree = nullptr;
    RpuFreeFn m_rpuFree = nullptr;
};

#endif  // DOVI_LIB_H_
