// The mime/multipart package: MIME multipart bodies (RFC 2046), as HTML
// forms post them (multipart/form-data, RFC 7578), ported from Go 1.27.3.
//
//     // Reading: the parts one at a time, or the whole form.
//     auto r = multipart::NewReader(body, params["boundary"]);
//     for (;;) {
//         auto [part, err] = r->NextPart();
//         if (err == io::EOF_) break;
//         part->FormName();  part->FileName();  io::ReadAll(*part);
//     }
//     auto [form, err] = r->ReadForm(32 << 20);   // files over 32 MiB on disk
//
//     // Writing:
//     auto w = multipart::NewWriter(buf);
//     w->WriteField("name", "gopher");
//     auto [fw, err] = w->CreateFormFile("upload", "notes.txt");
//     io::Copy(*fw, *file);
//     w->Close();
//     http::Post(url, w->FormDataContentType(), buf);
//
// To protect against malicious inputs, a part may have at most 10000 header
// lines, and ReadForm reads at most 1000 parts and 10000 header lines of
// files in all, as Go's defaults are.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net/textproto.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <functional>
#include <map>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::multipart {

// multipart.ErrMessageTooLarge: a form, or a part's header, is over the
// limits.
extern const error ErrMessageTooLarge;

class Reader;

// multipart.Part: a part of a multipart body. Reading it reads its body;
// a quoted-printable body (Content-Transfer-Encoding: quoted-printable)
// is decoded, and the header removed, unless the part came from
// NextRawPart. The part is valid until the next NextPart.
class Part final : public io::ReadCloser {
public:
    Part(ptr<multipart::Reader> mr, uint64 gen, textproto::MIMEHeader header, bool rawPart);
    ~Part() override;

    // The header of the part, with canonical keys.
    textproto::MIMEHeader Header;

    std::pair<isize, error> Read(std::span<byte> p) override;
    // Discards the rest of the part's body.
    error Close() override;

    // The name parameter of a form-data Content-Disposition, or "".
    string FormName();
    // The filename parameter of the Content-Disposition, as its base name
    // (filepath::Base), or "".
    string FileName();

private:
    void parseContentDisposition();

    ptr<io::Reader> r_;
    bool parsed_ = false;
    string disposition_;
    std::map<string, string> dispositionParams_;
};

// multipart.File: an uploaded file of a Form, in memory or on disk.
struct File : public io::Reader, public io::ReaderAt, public io::Seeker, public io::Closer {};

// multipart.FileHeader: a file part of a Form.
class FileHeader final : public object {
public:
    string Filename;
    textproto::MIMEHeader Header;
    int64 Size = 0;

    // Opens the file for reading.
    std::pair<ptr<File>, error> Open();

private:
    friend class Reader;
    friend class Form;
    slice<byte> content_;  // the file, if it is in memory
    string tmpfile_;       // the temporary file it is in, if on disk
    int64 tmpoff_ = 0;     // its offset there
};

// multipart.Form: a parsed multipart form: the values of its fields, and
// its files, by name. Files on disk stay until RemoveAll.
class Form final : public object {
public:
    std::map<string, std::vector<string>, std::less<>> Value;
    std::map<string, std::vector<ptr<FileHeader>>, std::less<>> File;

    // Removes the temporary files of the form.
    error RemoveAll();
};

// multipart.Reader: iterates over the parts of a multipart body.
class Reader final : public object {
public:
    Reader(ptr<io::Reader> r, std::string_view boundary);
    ~Reader() override;

    // The next part, or io::EOF_ after the last. The previous part's body
    // is skipped. A quoted-printable part is decoded.
    std::pair<ptr<Part>, error> NextPart();
    // NextPart without decoding the Content-Transfer-Encoding.
    std::pair<ptr<Part>, error> NextRawPart();
    // Parses a whole multipart/form-data body. Up to maxMemory bytes of
    // files are kept in memory, plus 10 MiB of non-file parts (and the
    // headers); larger files are stored in temporary files on disk. Past
    // those limits, ErrMessageTooLarge. Call the Form's RemoveAll.
    std::pair<ptr<Form>, error> ReadForm(int64 maxMemory);

    // The directory of ReadForm's temporary files; os::TempDir() if empty.
    string tempDir;

private:
    friend class partReader;
    friend class Part;
    std::pair<ptr<Part>, error> nextPart(bool rawPart, int64 maxMIMEHeaderSize, int64 maxMIMEHeaders);
    bool isFinalBoundary(std::span<const byte> line);
    bool isBoundaryDelimiterLine(std::span<const byte> line);
    std::pair<isize, error> readPart(uint64 gen, std::span<byte> d);
    void closePart();

    ptr<bufio::Reader> bufReader_;
    int partsRead_ = 0;
    std::string boundary_;  // "\r\n--boundary--"
    std::string_view nl_;                // "\r\n" or "\n" (set after seeing first boundary line)
    std::string_view nlDashBoundary_;    // nl + "--boundary"
    std::string_view dashBoundaryDash_;  // "--boundary--"
    std::string_view dashBoundary_;      // "--boundary"

    // The state of the current part.
    uint64 gen_ = 0;    // the current part's generation; parts of others read EOF
    bool inPart_ = false;
    isize n_ = 0;       // known data bytes waiting in bufReader_
    int64 total_ = 0;   // total data bytes read already
    error err_;         // error to return when n_ == 0
    error readErr_;     // read error observed from bufReader_
};

// multipart.NewReader: a Reader of the parts of r, separated by boundary,
// the boundary parameter of the Content-Type ("multipart/form-data;
// boundary=...").
ptr<Reader> NewReader(ptr<io::Reader> r, std::string_view boundary);

// multipart.Writer: writes a multipart body to w.
class Writer final : public object {
public:
    explicit Writer(ptr<io::Writer> w);
    ~Writer() override;

    // The boundary, random unless set.
    string Boundary() const { return boundary_; }
    // Sets the boundary, which must be 1 to 70 characters of RFC 2046's
    // bchars, before anything is written.
    error SetBoundary(std::string_view boundary);
    // "multipart/form-data; boundary=...", for an HTTP Content-Type.
    string FormDataContentType() const;
    // Starts a part with header, ending the previous one. Write the part's
    // body to the returned Writer.
    std::pair<ptr<io::Writer>, error> CreatePart(const textproto::MIMEHeader& header);
    // Starts a form-data file part: name fieldname, filename filename,
    // Content-Type application/octet-stream.
    std::pair<ptr<io::Writer>, error> CreateFormFile(std::string_view fieldname, std::string_view filename);
    // Starts a form-data part named fieldname.
    std::pair<ptr<io::Writer>, error> CreateFormField(std::string_view fieldname);
    // Writes a form-data field with its value.
    error WriteField(std::string_view fieldname, std::string_view value);
    // Ends the last part and writes the final boundary.
    error Close();

    struct part;

private:
    ptr<io::Writer> w_;
    string boundary_;
    ptr<part> lastpart_;
};

// multipart.NewWriter: a Writer to w with a random boundary.
ptr<Writer> NewWriter(ptr<io::Writer> w);

// multipart.FileContentDisposition: the Content-Disposition of a form-data
// file part: form-data; name="fieldname"; filename="filename".
string FileContentDisposition(std::string_view fieldname, std::string_view filename);

}  // namespace gopp::multipart
