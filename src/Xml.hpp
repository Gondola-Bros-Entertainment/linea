// linea - a small XML 1.0 parser with namespaces, for reading SVG. Internal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace linea::detail {

// The namespaces that reading SVG tells apart. Any other is foreign, and its elements and attributes are ignored.
enum class XmlNamespace : std::uint8_t { none, svg, xlink, xml, xhtml, linea, other };

inline constexpr std::string_view svgNamespaceUri = "http://www.w3.org/2000/svg";
inline constexpr std::string_view xlinkNamespaceUri = "http://www.w3.org/1999/xlink";
inline constexpr std::string_view xmlNamespaceUri = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view xhtmlNamespaceUri = "http://www.w3.org/1999/xhtml";
// The namespace of the attributes that toSvg() writes for fromSvg() alone, as Inkscape writes inkscape:original-d.
inline constexpr std::string_view lineaNamespaceUri = "https://github.com/Gondola-Bros-Entertainment/linea";

struct XmlAttribute {
    XmlNamespace space = XmlNamespace::none;
    // The local name, without a prefix.
    std::string name;
    // The value with its references replaced and its white space normalized, as XML 1.0 section 3.3.3 specifies.
    std::string value;
};

struct XmlElement {
    XmlNamespace space = XmlNamespace::none;
    // The local name, without a prefix.
    std::string name;
    // Every attribute but the namespace declarations, in document order.
    std::vector<XmlAttribute> attributes;
    std::vector<XmlElement> children;
    // Whether the element holds character data other than white space, directly or in a CDATA section.
    bool hasText = false;
    // Where the element's start tag begins, in bytes from the start of the document.
    std::size_t offset = 0;

    // The value of the attribute @p name in namespace @p space, or nullptr when the element has none.
    [[nodiscard]] const std::string *attribute(std::string_view name, XmlNamespace space = XmlNamespace::none) const;
};

// The root element of the UTF-8 XML 1.0 document @p text, read with XML namespaces. Comments, processing instructions,
// an XML declaration and a DOCTYPE without an internal subset are read and dropped. Throws SvgError, at the line and
// column where the document goes wrong, for a document that is not well-formed or not namespace-well-formed, for one in
// an encoding other than UTF-8, for a DOCTYPE with an internal subset, which can declare entities, and for elements
// that nest deeper than deepestSvgNesting.
XmlElement parseXml(std::string_view text);

} // namespace linea::detail
