#ifndef T_TYPES_H_
#define T_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#define strcasecmp stricmp
#endif
char* strnstr(const char* s1, const char* s2, size_t len);

uint64_t my_ntohll(const uint64_t& original);
uint64_t my_htonll(const uint64_t& original);
uint32_t my_ntohl(uint32_t val);
uint16_t my_ntohs(uint16_t val);
#define my_htonl(val) my_ntohl(val)
#define my_htons(val) my_ntohs(val)

int64_t strToInt64(const char*);
uint64_t strToInt64u(const char*);
int32_t strToInt32(const char*);
int32_t strToInt32(const std::string&);
int32_t strToInt32(const char*, int radix);
uint32_t strToInt32u(const char*, int radix = 10);
int16_t strToInt16(const char*);
uint16_t strToInt16u(const char*);
int8_t strToInt8(const char*);
uint8_t strToInt8u(const char*);
double strToDouble(const char*);  // The length of the string should not exceed 15 characters
float strToFloat(const char*);    // The length of the string should not exceed 15 characters
bool strToBool(const char*);
bool strEndWith(const std::string& str, const std::string& substr);
bool strStartWith(const std::string& str, const std::string& substr);
std::string strPadLeft(const std::string& str, size_t newSize, char filler);
std::string strPadRight(const std::string& str, size_t newSize, char filler);

std::vector<std::string> splitStr(const char* str, char splitter);
std::vector<std::string> splitStr(const std::string& str, const std::string& splitter);
void splitStr(std::vector<std::string>& rez, const char* str, char splitter);

std::string trimStr(const std::string& value);

std::string extractFileExt(const std::string& src);
std::string extractFileName(const std::string& src);
std::string extractFileName2(const std::string& src, bool withExt = true);
std::string extractFilePath(const std::string& src);
std::string closeDirPath(const std::string& src, char delimiter = ' ');
bool isValidFileName(const std::string& src);

std::vector<std::string> splitQuotedStr(const char* str, char splitter);

std::string int64ToStr(const int64_t&);
std::string int64uToStr(const uint64_t&);
std::string int32ToStr(const int32_t&);
std::string int32uToStr(const uint32_t&);
std::string int32ToHex(const int32_t&);
std::string int32uToHex(const uint32_t&);
//! Conversion of a floating-point number into a string.
/*!
         When precision = -1, the output contains four digits after the comma.
*/
std::string doubleToStr(const double& x, int precision = -1);

std::string int16ToStr(const int16_t&);
std::string int16uToStr(const uint16_t&);
std::string int8ToStr(const int8_t&);
std::string int8uToStr(const uint8_t&);
void int8uToStr(const uint8_t&, char* buf);

std::string boolToStr(const bool&);

// Work on lines
std::string strToUpperCase(const std::string& src);
std::string strToLowerCase(const std::string& src);

enum class CaseType
{
    ctLower,
    ctUpper
};

template <typename Type>
class CaseChanger
{
   public:
    CaseChanger(const CaseType caseType = CaseType::ctLower) : m_case(caseType) {}

    int operator()(Type& elem) const { return 0; }

    int operator()(const char& elem) const { return m_case == CaseType::ctLower ? tolower(elem) : toupper(elem); }

   private:
    CaseType m_case;
};

typedef uint16_t ip_port_t;

uint32_t roundDown(const uint32_t& value, const uint32_t& roundVal);
uint32_t roundUp(const uint32_t& value, const uint32_t& roundVal);
int32_t roundUp(const int32_t& value, const int32_t& roundVal);

// 64-bit versions
uint64_t roundDown64(const uint64_t& value, const uint64_t& roundVal);
uint64_t roundUp64(const uint64_t& value, const uint64_t& roundVal);

uint32_t random32();

// Reads and writes of a 16-, 32- or 64-bit value at any byte address. In a stream or a file buffer such a value is
// rarely on a multiple of its size, and C++ does not allow it to be read or written through a T* there; memcpy is
// allowed, and compilers turn it into the same single load or store.
template <typename T>
T loadUnaligned(const void* p)
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
void storeUnaligned(void* p, const T v)
{
    std::memcpy(p, &v, sizeof v);
}

// A pointer to such values, for code written with a T*: *p, p[i], p++ and p < q as with a T*, each read and write
// through memcpy. The bytes read and written are the same as through a T*.
template <typename T>
class UnalignedPtr
{
   public:
    class Ref
    {
       public:
        explicit Ref(uint8_t* p) : m_p(p) {}
        Ref(const Ref&) = default;
        operator T() const { return loadUnaligned<T>(m_p); }
        Ref& operator=(const T v)
        {
            storeUnaligned<T>(m_p, v);
            return *this;
        }
        Ref& operator=(const Ref& r) { return *this = static_cast<T>(r); }
        Ref& operator+=(const T v) { return *this = static_cast<T>(static_cast<T>(*this) + v); }

       private:
        uint8_t* m_p;
    };

    UnalignedPtr() : m_p(nullptr) {}
    explicit UnalignedPtr(void* p) : m_p(static_cast<uint8_t*>(p)) {}
    Ref operator*() const { return Ref(m_p); }
    Ref operator[](const std::ptrdiff_t i) const { return Ref(m_p + i * static_cast<std::ptrdiff_t>(sizeof(T))); }
    UnalignedPtr& operator++()
    {
        m_p += sizeof(T);
        return *this;
    }
    UnalignedPtr operator++(int)
    {
        const UnalignedPtr prev = *this;
        m_p += sizeof(T);
        return prev;
    }
    bool operator<(const UnalignedPtr& other) const { return m_p < other.m_p; }
    [[nodiscard]] uint8_t* bytes() const { return m_p; }

   private:
    uint8_t* m_p;
};

#ifdef _WIN32
std::vector<wchar_t> fromAcp(const char*, int sz);
std::vector<wchar_t> toWide(const char*, int sz = -1);
std::vector<wchar_t> toWide(const std::string&);
std::string toUtf8(const wchar_t*);
#endif

#endif  //_T_TYPES_H_
