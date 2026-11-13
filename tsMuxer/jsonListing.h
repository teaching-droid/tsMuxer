// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef JSON_LISTING_H_
#define JSON_LISTING_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ** --json: A LISTING AS A JSON DOCUMENT. **
//
// What tsMuxeR lists of a source, the tracks of a file or a playlist and the titles of a disc, is text made to be
// read by a person: "Track ID:    4113", "Stream lang: eng", "Title 1  00000.mpls ...". A program that wants the same
// facts had to take these lines apart, and every change of a line for its reader broke it.
//
// With --json before the source the listing is ONE JSON document on the standard output, and nothing else is written
// there or to the error output: no version line, no notes, no warnings. What a listing would have said beside its
// facts is in "messages", line by line, each with its level. A listing that fails is a document as well, with
// "error" in it, and the exit code is the one the text listing ends with. Only a listing that is stopped on request
// (exit code 4) writes nothing: the program ends where it stands.
//
// The document says which form it has: "format": 1. A member is added without a new number; a member that changes its
// meaning or goes away raises it. Times are seconds, as numbers; a member without a value is left out.
namespace jsonListing
{
// A value of a document: a text, a number, true or false, a list, or an object whose members keep the order they were
// set in.
class Value
{
   public:
    // members of an object; one set twice keeps its place and takes the new value
    Value& text(const std::string& key, const std::string& value);
    Value& number(const std::string& key, int64_t value);
    Value& seconds(const std::string& key, double value);
    Value& flag(const std::string& key, bool value);
    Value& list(const std::string& key);    // the list of that name, made when it is not there
    Value& object(const std::string& key);  // the object of that name, made when it is not there
    [[nodiscard]] bool has(const std::string& key) const;

    // items of a list
    Value& addText(const std::string& value);
    Value& addNumber(int64_t value);
    Value& addSeconds(double value);
    Value& addObject();
    Value& addList();

    void write(std::string& out, int depth) const;

   private:
    enum class Kind
    {
        Object,
        List,
        Text,
        Literal  // a number, true or false, as it is written
    };
    Kind m_kind = Kind::Object;
    std::string m_text;
    std::vector<std::pair<std::string, Value>> m_members;
    std::vector<Value> m_items;

    Value& member(const std::string& key);
};

// --json was given: the listing is collected in doc() and written by finish()
extern bool active;

// Takes --json out of the arguments, wherever it stands before the source, and from then on keeps every line written to
// the standard output and the error output for "messages". Returns false when --json stands beside something that is
// no listing (a mux, --batch, --report, --make-sample, the help): the reason is kept for the document, which the
// caller writes with finish(), and the program is to end with a usage error.
bool takeOption(int& argc, char** argv, const char* version);

Value& doc();

// The reason a listing failed, for "error"; without one, finish() takes the last line of the error output.
void fail(const std::string& message);

// Writes the document to the standard output. With an exit code other than 0 it has "error": the code and the message.
void finish(int exitCode);

// A line of the text listing that the document gives as a member: written as before without --json, and not at all
// with it (it would only be a message that says the same again).
#define LISTLINE(msg)                \
    do                               \
    {                                \
        if (!jsonListing::active)    \
            LTRACE(LT_INFO, 2, msg); \
    } while (false)

}  // namespace jsonListing

#endif  // JSON_LISTING_H_
