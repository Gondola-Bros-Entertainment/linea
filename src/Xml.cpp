// linea - Xml implementation: a recursive descent over the productions of XML 1.0 (Fifth Edition), whose numbers the
// comments below cite, and Namespaces in XML 1.0 (Third Edition).
#include "Xml.hpp"

#include "SvgSyntax.hpp"
#include "linea/Color.hpp"
#include "linea/Svg.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>

namespace linea::detail {
namespace {

constexpr std::string_view byteOrderMark = "\xEF\xBB\xBF";

// UTF-8's well-formed byte sequences (the Unicode Standard, Table 3-7): a lead byte from first to last, then
// continuation more bytes, the first of them from low to high and the rest from 0x80 to 0xBF.
struct Utf8Lead {
    unsigned char first{};
    unsigned char last{};
    int continuation{};
    unsigned char low{};
    unsigned char high{};
};
constexpr std::array<Utf8Lead, 8> utf8Leads{{
    {0xC2, 0xDF, 1, 0x80, 0xBF},
    {0xE0, 0xE0, 2, 0xA0, 0xBF},
    {0xE1, 0xEC, 2, 0x80, 0xBF},
    {0xED, 0xED, 2, 0x80, 0x9F},
    {0xEE, 0xEF, 2, 0x80, 0xBF},
    {0xF0, 0xF0, 3, 0x90, 0xBF},
    {0xF1, 0xF3, 3, 0x80, 0xBF},
    {0xF4, 0xF4, 3, 0x80, 0x8F},
}};
constexpr unsigned char continuationLow = 0x80;
constexpr unsigned char continuationHigh = 0xBF;
constexpr unsigned char asciiEnd = 0x80;
constexpr int continuationBits = 6;
constexpr char32_t continuationPayload = 0x3F;
// A lead byte before n continuation bytes carries 6 - n bits of the code point.
constexpr int leadBits = 6;

// The ranges of Unicode code points that XML 1.0 allows in a document (production 2, Char) beyond tab, line feed and
// carriage return.
constexpr char32_t tab = 0x09;
constexpr char32_t lineFeed = 0x0A;
constexpr char32_t carriageReturn = 0x0D;
constexpr char32_t firstPrintable = 0x20;
constexpr char32_t surrogatesStart = 0xD800;
constexpr char32_t surrogatesEnd = 0xDFFF;
constexpr char32_t lastBasic = 0xFFFD;
constexpr char32_t firstSupplementary = 0x10000;
constexpr char32_t lastCodePoint = 0x10FFFF;

constexpr bool isXmlChar(char32_t c) {
    return c == tab || c == lineFeed || c == carriageReturn || (c >= firstPrintable && c < surrogatesStart) ||
           (c > surrogatesEnd && c <= lastBasic) || (c >= firstSupplementary && c <= lastCodePoint);
}

// A code point's range, inclusive.
struct CodeRange {
    char32_t first{};
    char32_t last{};
};

// NameStartChar beyond ASCII (production 4).
constexpr std::array<CodeRange, 12> nameStartRanges{{
    {0xC0, 0xD6},
    {0xD8, 0xF6},
    {0xF8, 0x2FF},
    {0x370, 0x37D},
    {0x37F, 0x1FFF},
    {0x200C, 0x200D},
    {0x2070, 0x218F},
    {0x2C00, 0x2FEF},
    {0x3001, 0xD7FF},
    {0xF900, 0xFDCF},
    {0xFDF0, 0xFFFD},
    {0x10000, 0xEFFFF},
}};
// NameChar beyond NameStartChar and ASCII (production 4a).
constexpr std::array<CodeRange, 3> nameRanges{{{0xB7, 0xB7}, {0x300, 0x36F}, {0x203F, 0x2040}}};

constexpr bool inRanges(char32_t c, std::span<const CodeRange> ranges) {
    return std::ranges::any_of(ranges, [c](const CodeRange &range) { return c >= range.first && c <= range.last; });
}

constexpr bool isAsciiLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

constexpr bool isNameStart(char32_t c) {
    return isAsciiLetter(c) || c == ':' || c == '_' || inRanges(c, nameStartRanges);
}

constexpr bool isNameChar(char32_t c) {
    return isNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || inRanges(c, nameRanges);
}

// XML's markup delimiters.
constexpr std::string_view declarationOpen = "<?xml";
constexpr std::string_view instructionOpen = "<?";
constexpr std::string_view instructionClose = "?>";
constexpr std::string_view commentOpen = "<!--";
constexpr std::string_view commentClose = "-->";
constexpr std::string_view cdataOpen = "<![CDATA[";
constexpr std::string_view cdataClose = "]]>";
constexpr std::string_view doctypeOpen = "<!DOCTYPE";
constexpr std::string_view endTagOpen = "</";
constexpr std::string_view emptyTagClose = "/>";

// White space as XML counts it (production 3).
constexpr bool isXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Appends the code point @p c to @p out in UTF-8.
void appendUtf8(std::string &out, char32_t c) {
    constexpr char32_t oneByte = 0x80;
    constexpr char32_t twoBytes = 0x800;
    constexpr unsigned char twoByteLead = 0xC0;
    constexpr unsigned char threeByteLead = 0xE0;
    constexpr unsigned char fourByteLead = 0xF0;
    const auto put = [&](char32_t byte) { out += static_cast<char>(static_cast<unsigned char>(byte)); };
    const auto continuation = [&](int shift) { put(continuationLow | ((c >> shift) & continuationPayload)); };
    if (c < oneByte) {
        put(c);
    } else if (c < twoBytes) {
        put(twoByteLead | (c >> continuationBits));
        continuation(0);
    } else if (c < firstSupplementary) {
        put(threeByteLead | (c >> (2 * continuationBits)));
        continuation(continuationBits);
        continuation(0);
    } else {
        put(fourByteLead | (c >> (3 * continuationBits)));
        continuation(2 * continuationBits);
        continuation(continuationBits);
        continuation(0);
    }
}

XmlNamespace namespaceOf(std::string_view uri) {
    if (uri.empty())
        return XmlNamespace::none;
    if (uri == svgNamespaceUri)
        return XmlNamespace::svg;
    if (uri == xlinkNamespaceUri)
        return XmlNamespace::xlink;
    if (uri == xmlNamespaceUri)
        return XmlNamespace::xml;
    if (uri == xhtmlNamespaceUri)
        return XmlNamespace::xhtml;
    if (uri == lineaNamespaceUri)
        return XmlNamespace::linea;
    return XmlNamespace::other;
}

bool equalIgnoringAsciiCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(a) == lower(b);
    });
}

class XmlParser {
  public:
    explicit XmlParser(std::string_view text) : text_(text) {}

    XmlElement document() {
        if (text_.starts_with("\xFE\xFF") || text_.starts_with("\xFF\xFE"))
            fail("UTF-16 documents are not supported, only UTF-8", 0);
        if (text_.starts_with(byteOrderMark))
            at_ = byteOrderMark.size();
        checkCharacters();
        const std::size_t afterOpen = at_ + declarationOpen.size();
        if (lookingAt(declarationOpen) && afterOpen < text_.size() &&
            (isXmlSpace(text_[afterOpen]) || text_.substr(afterOpen).starts_with(instructionClose)))
            declaration();
        miscellany();
        if (lookingAt(doctypeOpen)) {
            doctype();
            miscellany();
        }
        if (!lookingAt("<") || lookingAt("<!") || lookingAt(instructionOpen) || lookingAt(endTagOpen))
            fail(at_ == text_.size() ? "the document has no root element" : "expected the root element", at_);
        XmlElement root = element(1);
        miscellany();
        if (at_ != text_.size())
            fail("only comments, processing instructions and white space may follow the root element", at_);
        return root;
    }

  private:
    // The characters of the document must be well-formed UTF-8, and each one XML allows (production 2).
    void checkCharacters() const {
        std::size_t i = at_;
        while (i < text_.size()) {
            const auto lead = static_cast<unsigned char>(text_[i]);
            char32_t c = lead;
            std::size_t length = 1;
            if (lead >= asciiEnd) {
                const auto found = std::ranges::find_if(
                    utf8Leads, [lead](const Utf8Lead &entry) { return lead >= entry.first && lead <= entry.last; });
                if (found == utf8Leads.end())
                    fail("the document is not well-formed UTF-8", i);
                c = lead & ((1u << (leadBits - found->continuation)) - 1u);
                for (int k = 1; k <= found->continuation; ++k) {
                    const std::size_t at = i + static_cast<std::size_t>(k);
                    const unsigned char byte = at < text_.size() ? static_cast<unsigned char>(text_[at]) : 0;
                    const unsigned char low = k == 1 ? found->low : continuationLow;
                    const unsigned char high = k == 1 ? found->high : continuationHigh;
                    if (byte < low || byte > high)
                        fail("the document is not well-formed UTF-8", i);
                    c = c << continuationBits | (byte & continuationPayload);
                }
                length += static_cast<std::size_t>(found->continuation);
            }
            if (!isXmlChar(c))
                fail("the document holds a character that XML does not allow", i);
            i += length;
        }
    }

    // The XML declaration (production 23): version, then optionally encoding and standalone.
    void declaration() {
        const std::size_t start = at_;
        at_ += declarationOpen.size();
        const std::optional<std::string> version = pseudoAttribute("version");
        if (!version || version->size() < 3 || !version->starts_with("1.") ||
            !std::ranges::all_of(version->substr(2), [](char c) { return isDigit(c); }))
            fail("the XML declaration needs a version of 1.x", start);
        if (const std::optional<std::string> encoding = pseudoAttribute("encoding")) {
            if (!equalIgnoringAsciiCase(*encoding, "UTF-8") && !equalIgnoringAsciiCase(*encoding, "US-ASCII"))
                fail("only UTF-8 documents are supported, not " + *encoding, start);
        }
        if (const std::optional<std::string> standalone = pseudoAttribute("standalone")) {
            if (*standalone != "yes" && *standalone != "no")
                fail("standalone must be yes or no", start);
        }
        skipSpace();
        expect(instructionClose, "the XML declaration does not end with ?>");
    }

    // A pseudo-attribute of the XML declaration, when the next one is named @p name.
    std::optional<std::string> pseudoAttribute(std::string_view name) {
        const std::size_t start = at_;
        if (!skipSpace() || !lookingAt(name)) {
            at_ = start;
            return std::nullopt;
        }
        at_ += name.size();
        skipSpace();
        expect("=", "expected '=' in the XML declaration");
        skipSpace();
        return quoted();
    }

    // A quoted literal, in single or double quotes, with no references.
    std::string quoted() {
        if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\''))
            fail("expected a quoted value", at_);
        const char quote = text_[at_];
        const std::size_t end = text_.find(quote, at_ + 1);
        if (end == std::string_view::npos)
            fail("a quoted value is not closed", at_);
        std::string value(text_.substr(at_ + 1, end - at_ - 1));
        at_ = end + 1;
        return value;
    }

    // A DOCTYPE without an internal subset (production 28): a name and an optional external identifier.
    void doctype() {
        const std::size_t start = at_;
        at_ += doctypeOpen.size();
        if (!skipSpace())
            fail("expected white space after <!DOCTYPE", at_);
        static_cast<void>(name());
        const bool spaced = skipSpace();
        if (spaced && (lookingAt("SYSTEM") || lookingAt("PUBLIC"))) {
            const bool isPublic = lookingAt("PUBLIC");
            at_ += std::string_view("SYSTEM").size();
            if (!skipSpace())
                fail("expected white space in the DOCTYPE", at_);
            static_cast<void>(quoted());
            if (isPublic) {
                if (!skipSpace())
                    fail("expected white space in the DOCTYPE", at_);
                static_cast<void>(quoted());
            }
            skipSpace();
        }
        if (lookingAt("["))
            fail("a DOCTYPE with an internal subset is not supported: it can declare entities", start);
        expect(">", "the DOCTYPE does not end with '>'");
    }

    // Comments, processing instructions and white space (production 27).
    void miscellany() {
        while (true) {
            skipSpace();
            if (lookingAt(commentOpen))
                comment();
            else if (lookingAt(instructionOpen))
                processingInstruction();
            else
                return;
        }
    }

    // A comment (production 15), in which -- may not appear.
    void comment() {
        const std::size_t start = at_;
        const std::size_t dashes = text_.find("--", at_ + commentOpen.size());
        if (dashes == std::string_view::npos)
            fail("a comment is not closed", start);
        if (!text_.substr(dashes).starts_with(commentClose))
            fail("'--' may only end a comment", dashes);
        at_ = dashes + commentClose.size();
    }

    // A processing instruction (production 16), whose target may not be xml.
    void processingInstruction() {
        const std::size_t start = at_;
        at_ += instructionOpen.size();
        const std::string target = name();
        if (equalIgnoringAsciiCase(target, "xml"))
            fail("an XML declaration may only start the document", start);
        if (!lookingAt(instructionClose) && !skipSpace())
            fail("expected white space after the processing instruction's target", at_);
        const std::size_t end = text_.find(instructionClose, at_);
        if (end == std::string_view::npos)
            fail("a processing instruction is not closed", start);
        at_ = end + instructionClose.size();
    }

    // An element and everything in it (production 39), with the namespaces that its attributes declare in scope.
    XmlElement element(int depth) {
        if (depth > deepestSvgNesting)
            fail("elements nest more than " + std::to_string(deepestSvgNesting) + " deep", at_);
        XmlElement result;
        result.offset = at_;
        ++at_;
        const std::string qualified = name();
        std::vector<std::pair<std::string, std::string>> attributes;
        std::vector<std::size_t> attributeOffsets;
        while (true) {
            const bool spaced = skipSpace();
            if (lookingAt(emptyTagClose) || lookingAt(">"))
                break;
            if (at_ >= text_.size())
                fail("the start tag of <" + qualified + "> is not closed", result.offset);
            if (!spaced)
                fail("expected white space before an attribute", at_);
            const std::size_t attributeAt = at_;
            std::string attributeName = name();
            skipSpace();
            expect("=", "expected '=' after the attribute " + attributeName);
            skipSpace();
            std::string value = attributeValue();
            if (std::ranges::any_of(attributes, [&](const auto &other) { return other.first == attributeName; }))
                fail("the attribute " + attributeName + " appears twice", attributeAt);
            attributes.emplace_back(std::move(attributeName), std::move(value));
            attributeOffsets.push_back(attributeAt);
        }

        const std::size_t scope = bindings_.size();
        for (std::size_t i = 0; i < attributes.size(); ++i)
            declare(attributes[i].first, attributes[i].second, attributeOffsets[i]);
        // An unprefixed element is in the default namespace, if one is declared.
        auto [prefix, local] = split(qualified, result.offset);
        result.space = namespaceOf(prefix.empty() ? boundUri("").value_or("") : uriBoundTo(prefix, result.offset));
        result.name = std::move(local);
        // Two attributes are one when their local names and namespace URIs match, whatever their prefixes (Namespaces
        // in XML, section 6.3).
        std::vector<std::pair<std::string_view, std::string>> expanded;
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            auto &[attributeName, value] = attributes[i];
            if (attributeName == "xmlns" || attributeName.starts_with("xmlns:"))
                continue;
            auto [attributePrefix, attributeLocal] = split(attributeName, attributeOffsets[i]);
            const std::string_view uri =
                attributePrefix.empty() ? std::string_view{} : uriBoundTo(attributePrefix, attributeOffsets[i]);
            if (std::ranges::any_of(
                    expanded, [&](const auto &other) { return other.first == uri && other.second == attributeLocal; }))
                fail("the attribute " + attributeName + " appears twice", attributeOffsets[i]);
            expanded.emplace_back(uri, attributeLocal);
            result.attributes.push_back({namespaceOf(uri), std::move(attributeLocal), std::move(value)});
        }

        if (lookingAt(emptyTagClose)) {
            at_ += emptyTagClose.size();
        } else {
            ++at_;
            content(result, qualified, depth);
        }
        bindings_.resize(scope);
        return result;
    }

    // What lies between an element's start and end tags (production 43), up to and including the end tag.
    void content(XmlElement &parent, const std::string &qualified, int depth) {
        while (true) {
            if (at_ >= text_.size())
                fail("<" + qualified + "> is not closed", parent.offset);
            if (lookingAt(endTagOpen)) {
                const std::size_t start = at_;
                at_ += endTagOpen.size();
                if (name() != qualified)
                    fail("</" + qualified + "> was expected here", start);
                skipSpace();
                expect(">", "the end tag of <" + qualified + "> does not end with '>'");
                return;
            }
            if (lookingAt(commentOpen)) {
                comment();
            } else if (lookingAt(cdataOpen)) {
                const std::size_t start = at_ + cdataOpen.size();
                const std::size_t end = text_.find(cdataClose, start);
                if (end == std::string_view::npos)
                    fail("a CDATA section is not closed", at_);
                parent.hasText = parent.hasText || !std::ranges::all_of(text_.substr(start, end - start), isXmlSpace);
                at_ = end + cdataClose.size();
            } else if (lookingAt(instructionOpen)) {
                processingInstruction();
            } else if (lookingAt("<!")) {
                fail("unexpected markup in <" + qualified + ">", at_);
            } else if (lookingAt("<")) {
                parent.children.push_back(element(depth + 1));
            } else if (lookingAt("&")) {
                std::string replaced;
                reference(replaced);
                parent.hasText = parent.hasText || !std::ranges::all_of(replaced, isXmlSpace);
            } else {
                const std::size_t end = std::min(text_.find_first_of("<&", at_), text_.size());
                const std::string_view data = text_.substr(at_, end - at_);
                if (const std::size_t marker = data.find(cdataClose); marker != std::string_view::npos)
                    fail("']]>' may not appear in text", at_ + marker);
                parent.hasText = parent.hasText || !std::ranges::all_of(data, isXmlSpace);
                at_ = end;
            }
        }
    }

    // An attribute's value (production 10), its references replaced and its white space normalized: each tab, line
    // feed and carriage return becomes a space, and a carriage return and line feed together become one.
    std::string attributeValue() {
        if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\''))
            fail("expected a quoted attribute value", at_);
        const std::size_t start = at_;
        const char quote = text_[at_++];
        std::string value;
        while (true) {
            if (at_ >= text_.size())
                fail("an attribute value is not closed", start);
            const char c = text_[at_];
            if (c == quote) {
                ++at_;
                return value;
            }
            if (c == '<')
                fail("'<' may not appear in an attribute value", at_);
            if (c == '&') {
                reference(value);
                continue;
            }
            if (c == '\r' && at_ + 1 < text_.size() && text_[at_ + 1] == '\n')
                ++at_;
            value += isXmlSpace(c) ? ' ' : c;
            ++at_;
        }
    }

    // Appends what the reference at the current place stands for (production 67): one of the five predefined
    // entities, or a character reference to a character that XML allows.
    void reference(std::string &out) {
        const std::size_t start = at_;
        const std::size_t end = text_.find(';', at_);
        if (end == std::string_view::npos)
            fail("a reference does not end with ';'", start);
        const std::string_view body = text_.substr(at_ + 1, end - at_ - 1);
        at_ = end + 1;
        if (body.starts_with('#')) {
            const bool hex = body.starts_with("#x");
            const std::string_view digits = body.substr(hex ? 2 : 1);
            const int base = hex ? 16 : 10;
            char32_t code = 0;
            for (const char digit : digits) {
                const std::optional<int> value = hexDigitValue(digit);
                if (!value || *value >= base)
                    fail("a character reference has a digit that is not one", start);
                code = code * static_cast<char32_t>(base) + static_cast<char32_t>(*value);
                if (code > lastCodePoint)
                    break;
            }
            if (digits.empty() || !isXmlChar(code))
                fail("a character reference to a character that XML does not allow", start);
            appendUtf8(out, code);
            return;
        }
        static constexpr std::array<std::pair<std::string_view, char>, 5> predefined{
            {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}}};
        const auto found = std::ranges::find(predefined, body, &std::pair<std::string_view, char>::first);
        if (found == predefined.end())
            fail("the entity &" + std::string(body) + "; is not defined; only XML's five predefined entities are",
                 start);
        out += found->second;
    }

    // An XML name (production 5).
    std::string name() {
        const std::size_t start = at_;
        bool first = true;
        while (at_ < text_.size()) {
            std::size_t length = 1;
            const char32_t c = codePointAt(at_, length);
            if (!(first ? isNameStart(c) : isNameChar(c)))
                break;
            at_ += length;
            first = false;
        }
        if (first)
            fail("expected a name", start);
        return std::string(text_.substr(start, at_ - start));
    }

    // The code point at @p at, its length in bytes in @p length. The document's UTF-8 is known to be well-formed.
    char32_t codePointAt(std::size_t at, std::size_t &length) const {
        const auto lead = static_cast<unsigned char>(text_[at]);
        if (lead < asciiEnd)
            return lead;
        const auto found = std::ranges::find_if(
            utf8Leads, [lead](const Utf8Lead &entry) { return lead >= entry.first && lead <= entry.last; });
        char32_t c = lead & ((1u << (leadBits - found->continuation)) - 1u);
        for (int k = 1; k <= found->continuation; ++k)
            c = c << continuationBits |
                (static_cast<unsigned char>(text_[at + static_cast<std::size_t>(k)]) & continuationPayload);
        length = 1 + static_cast<std::size_t>(found->continuation);
        return c;
    }

    // A qualified name's prefix and local part (Namespaces in XML, production 7), each without a colon.
    std::pair<std::string, std::string> split(const std::string &qualified, std::size_t at) const {
        const std::size_t colon = qualified.find(':');
        if (colon == std::string::npos)
            return {"", qualified};
        if (colon == 0 || colon + 1 == qualified.size() || qualified.find(':', colon + 1) != std::string::npos)
            fail(qualified + " is not a qualified name", at);
        return {qualified.substr(0, colon), qualified.substr(colon + 1)};
    }

    // Binds the namespace that the attribute @p name declares, if it declares one.
    void declare(const std::string &name, const std::string &uri, std::size_t at) {
        if (name == "xmlns") {
            if (uri == xmlNamespaceUri || uri == "http://www.w3.org/2000/xmlns/")
                fail("the default namespace may not be the xml or xmlns namespace", at);
            bindings_.emplace_back("", uri);
        } else if (name.starts_with("xmlns:")) {
            const std::string prefix = name.substr(std::string_view("xmlns:").size());
            if (prefix.empty() || prefix.find(':') != std::string::npos)
                fail(name + " is not a qualified name", at);
            if (uri.empty())
                fail("the prefix " + prefix + " may not be bound to no namespace", at);
            if ((prefix == "xml") != (uri == xmlNamespaceUri) || prefix == "xmlns")
                fail("the prefix " + prefix + " may not be bound to " + uri, at);
            bindings_.emplace_back(prefix, uri);
        }
    }

    // The namespace URI that @p prefix is bound to, the innermost binding first, or nothing when it is not bound.
    std::optional<std::string_view> boundUri(std::string_view prefix) const {
        if (prefix == "xml")
            return xmlNamespaceUri;
        for (auto binding = bindings_.rbegin(); binding != bindings_.rend(); ++binding)
            if (binding->first == prefix)
                return binding->second;
        return std::nullopt;
    }

    // The namespace URI that the declared @p prefix is bound to.
    std::string_view uriBoundTo(const std::string &prefix, std::size_t at) const {
        const std::optional<std::string_view> uri = boundUri(prefix);
        if (!uri)
            fail("the namespace prefix " + prefix + " is not declared", at);
        return *uri;
    }

    bool lookingAt(std::string_view what) const { return text_.substr(std::min(at_, text_.size())).starts_with(what); }

    void expect(std::string_view what, const std::string &problem) {
        if (!lookingAt(what))
            fail(problem, at_);
        at_ += what.size();
    }

    // Skips white space. Whether there was any.
    bool skipSpace() {
        const std::size_t start = at_;
        while (at_ < text_.size() && isXmlSpace(text_[at_]))
            ++at_;
        return at_ > start;
    }

    [[noreturn]] void fail(const std::string &problem, std::size_t at) const {
        const TextPosition position = positionOf(text_, at);
        throw SvgError(problem, position.line, position.column);
    }

    std::string_view text_;
    std::size_t at_ = 0;
    // The namespace prefixes in scope, innermost last, each with its URI; the default namespace has no prefix.
    std::vector<std::pair<std::string, std::string>> bindings_;
};

} // namespace

const std::string *XmlElement::attribute(std::string_view attributeName, XmlNamespace attributeSpace) const {
    for (const XmlAttribute &candidate : attributes)
        if (candidate.space == attributeSpace && candidate.name == attributeName)
            return &candidate.value;
    return nullptr;
}

XmlElement parseXml(std::string_view text) { return XmlParser(text).document(); }

} // namespace linea::detail
