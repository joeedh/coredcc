#include "coredcc/layout/xml.h"

#include "coredcc/base/text.h"
#include "coredcc/layout/draft.h"

#include <string>
#include <utility>
#include <vector>

namespace coredcc {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

bool isSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool hasText(LayoutKind kind)
{
  return kind == LayoutKind::Label || kind == LayoutKind::Tool || kind == LayoutKind::Custom;
}

std::string trimmed(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) {
    a++;
  }
  while (b > a && isSpace(s[b - 1])) {
    b--;
  }
  return s.substr(a, b - a);
}

std::string collapsed(const std::string &s)
{
  std::string out;
  for (char c : trimmed(s)) {
    if (!isSpace(c)) {
      out += c;
    }
    else if (out.back() != ' ') {
      out += ' ';
    }
  }
  return out;
}

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  size_t offset = 0;
  Status error;

  bool run(std::vector<DraftNode> &out)
  {
    if (s_.size() > LayoutLimits::kMaxBytes) {
      return fail(LayoutLimits::kMaxBytes, "the text is longer than 1 MiB",
                  StatusCode::OutOfRange);
    }
    const char *what = nullptr;
    if (const size_t at = layout_detail::firstBadByte(s_, what); at < s_.size()) {
      return fail(at, what);
    }
    if (starts(kBom)) {
      at_ += kBom.size();
    }
    if (starts("<?xml") && at_ + 5 < s_.size() && (isSpace(s_[at_ + 5]) || s_[at_ + 5] == '?') &&
        !declaration())
    {
      return false;
    }
    while (at_ < s_.size()) {
      if (s_[at_] == '<' ? !markup() : !text()) {
        return false;
      }
    }
    if (!open_.empty()) {
      const LayoutPosition from = layoutPosition(s_, nodes_[open_.back().node].offset);
      return fail(at_, "the <" + std::string(open_.back().name) + "> in line " +
                           std::to_string(from.line) + " isn't closed");
    }
    if (nodes_.empty()) {
      return fail(at_, "there is no <layout>");
    }
    out = std::move(nodes_);
    return true;
  }

 private:
  struct Open {
    int node;
    std::string_view name;
    std::string text;
    /** Of the first character of the text that isn't space. */
    size_t textOffset = 0;
    bool hasText = false;
  };
  struct Attr {
    std::string_view name;
    std::string value;
    size_t offset;
    size_t valueOffset;
    bool verbatim;
  };

  bool fail(size_t at, const std::string &what, StatusCode code = StatusCode::InvalidArgument)
  {
    return fail(at, Status::error(code, what.c_str()));
  }
  bool fail(size_t at, Status s)
  {
    offset = at;
    error = std::move(s);
    return false;
  }

  bool starts(std::string_view with) const
  {
    return s_.substr(at_, with.size()) == with;
  }
  bool skipSpace()
  {
    const size_t from = at_;
    while (at_ < s_.size() && isSpace(s_[at_])) {
      at_++;
    }
    return at_ > from;
  }
  /* "'x'", or "the end". */
  std::string here() const
  {
    if (at_ >= s_.size()) {
      return "the end of the text";
    }
    size_t n = 1; /* the text is valid UTF-8 */
    while (at_ + n < s_.size() && (uint8_t(s_[at_ + n]) & 0xC0) == 0x80) {
      n++;
    }
    return "'" + std::string(s_.substr(at_, n)) + "'";
  }

  bool name(std::string_view &out, const char *of)
  {
    const size_t from = at_;
    while (at_ < s_.size() && layout_detail::isXmlNameChar(s_[at_], at_ == from)) {
      at_++;
    }
    out = s_.substr(from, at_ - from);
    if (at_ < s_.size() && s_[at_] == ':') {
      return fail(at_, "namespaces aren't supported");
    }
    if (out.empty()) {
      return fail(at_, std::string("expected the name of an ") + of + ", not " + here());
    }
    return true;
  }

  /* An entity, at its '&'. */
  bool entity(std::string &out)
  {
    static constexpr struct {
      std::string_view text;
      char c;
    } kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
    for (const auto &e : kEntities) {
      if (starts(e.text)) {
        out += e.c;
        at_ += e.text.size();
        return true;
      }
    }
    if (starts("&#")) {
      return fail(at_, "numeric references aren't supported: write the character itself");
    }
    return fail(at_, "'&' must start one of &lt; &gt; &amp; &quot; &apos;");
  }

  /* name="value", with the space before it skipped. */
  bool attribute(Attr &out)
  {
    out.offset = at_;
    if (!name(out.name, "attribute")) {
      return false;
    }
    skipSpace();
    if (at_ >= s_.size() || s_[at_] != '=') {
      return fail(at_, "expected '=' after '" + std::string(out.name) + "', not " + here());
    }
    at_++;
    skipSpace();
    if (at_ >= s_.size() || (s_[at_] != '"' && s_[at_] != '\'')) {
      return fail(at_, "expected a quote, not " + here());
    }
    const char quote = s_[at_++];
    out.valueOffset = at_;
    out.value.clear();
    while (at_ < s_.size() && s_[at_] != quote) {
      const char c = s_[at_];
      if (c == '<') {
        return fail(at_, "'<' in a value is written &lt;");
      }
      if (c == '&') {
        if (!entity(out.value)) {
          return false;
        }
        continue;
      }
      at_ += c == '\r' && at_ + 1 < s_.size() && s_[at_ + 1] == '\n' ? 2 : 1;
      out.value += isSpace(c) ? ' ' : c;
    }
    if (at_ >= s_.size()) {
      return fail(out.valueOffset - 1, "the value isn't closed");
    }
    out.verbatim = at_ - out.valueOffset == out.value.size();
    at_++;
    return true;
  }

  bool declaration()
  {
    at_ += 5;
    bool version = false;
    while (true) {
      const bool space = skipSpace();
      if (starts("?>")) {
        at_ += 2;
        return version || fail(at_ - 2, "the <?xml?> declaration has no version");
      }
      if (!space) {
        return fail(at_, "expected '?>', not " + here());
      }
      Attr a;
      if (!attribute(a)) {
        return false;
      }
      const std::string &v = a.value;
      const bool utf8 = v.size() == 5 && (v[0] | 32) == 'u' && (v[1] | 32) == 't' &&
                        (v[2] | 32) == 'f' && v.substr(3) == "-8";
      if (a.name == "version" ? v != "1.0" :
          a.name == "encoding" ? !utf8 :
                                 a.name != "standalone")
      {
        return fail(a.offset, "'" + std::string(a.name) + "=" + v + "' isn't supported");
      }
      version |= a.name == "version";
    }
  }

  bool comment()
  {
    const size_t from = at_;
    const size_t end = s_.find("--", at_ + 4);
    if (end == std::string_view::npos) {
      return fail(from, "the comment isn't closed");
    }
    if (s_.substr(end, 3) != "-->") {
      return fail(end, "'--' can't be in a comment");
    }
    at_ = end + 3;
    return true;
  }

  bool markup()
  {
    if (starts("<!--")) {
      return comment();
    }
    if (starts("<![CDATA[")) {
      return fail(at_, "CDATA isn't supported");
    }
    if (starts("<!")) {
      return fail(at_, "DOCTYPE and other declarations aren't supported");
    }
    if (starts("<?")) {
      return fail(at_, starts("<?xml") ? "an <?xml?> declaration must come first" :
                                         "processing instructions aren't supported");
    }
    return starts("</") ? endTag() : startTag();
  }

  bool startTag()
  {
    const size_t from = at_++;
    std::string_view tag;
    if (!name(tag, "element")) {
      return false;
    }
    if (open_.empty() && !nodes_.empty()) {
      return fail(from, "there is more than one root element");
    }
    DraftNode n;
    n.offset = from;
    n.parent = open_.empty() ? -1 : open_.back().node;
    const bool inCustom = n.parent >= 0 && nodes_[n.parent].kind == LayoutKind::Custom;
    const bool known = layout_detail::isKindName(tag, n.kind);
    if (!known) {
      if (!inCustom) {
        Status s = Status::error(StatusCode::InvalidArgument,
                                 ("there is no element <" + std::string(tag) + ">").c_str());
        Suggester suggest(tag);
        for (std::string_view k : layout_detail::kindNames()) {
          suggest.consider(k);
        }
        suggest.addTo(s);
        return fail(from, std::move(s));
      }
      n.kind = LayoutKind::Custom;
      DraftAttr a;
      a.id = LayoutAttrId::Tag;
      a.value = tag;
      a.offset = a.valueOffset = from + 1;
      n.attrs.push_back(std::move(a));
    }
    if (!attributes(n, known)) {
      return false;
    }
    const bool closed = s_[at_] == '/';
    at_ += closed ? 2 : 1;
    nodes_.push_back(std::move(n));
    Open o;
    o.node = int(nodes_.size()) - 1;
    o.name = tag;
    open_.push_back(std::move(o));
    return !closed || close();
  }

  /* Up to '>' or '/>'. */
  bool attributes(DraftNode &n, bool known)
  {
    while (true) {
      const bool space = skipSpace();
      if (at_ < s_.size() && (s_[at_] == '>' || starts("/>"))) {
        return true;
      }
      if (!space) {
        return fail(at_, "expected '>' or an attribute, not " + here());
      }
      if (n.attrs.size() == size_t(LayoutLimits::kMaxAttrs)) {
        return fail(at_, "an element has more than " + std::to_string(LayoutLimits::kMaxAttrs) +
                             " attributes", StatusCode::OutOfRange);
      }
      Attr a;
      if (!attribute(a)) {
        return false;
      }
      if (a.name == "xmlns") {
        return fail(a.offset, "namespaces aren't supported");
      }
      Result<LayoutAttrId> id = layout_detail::attrOf(n.kind, a.name);
      if (!id.ok()) {
        return fail(a.offset, id.status());
      }
      if (!known && *id == LayoutAttrId::Tag) {
        return fail(a.offset, "the element's name is its tag");
      }
      DraftAttr d;
      d.id = *id;
      d.name = *id == LayoutAttrId::Free ? a.name : std::string_view();
      if (n.find(d.id, d.name)) {
        return fail(a.offset, "'" + std::string(a.name) + "' is there twice");
      }
      d.value = std::move(a.value);
      d.offset = a.offset;
      d.valueOffset = a.valueOffset;
      d.verbatim = a.verbatim;
      n.attrs.push_back(std::move(d));
    }
  }

  bool endTag()
  {
    const size_t from = at_;
    at_ += 2;
    std::string_view tag;
    if (!name(tag, "element")) {
      return false;
    }
    skipSpace();
    if (at_ >= s_.size() || s_[at_] != '>') {
      return fail(at_, "expected '>', not " + here());
    }
    at_++;
    if (open_.empty()) {
      return fail(from, "there is nothing that </" + std::string(tag) + "> could close");
    }
    if (tag != open_.back().name) {
      return fail(from, "</" + std::string(tag) + "> can't close <" +
                            std::string(open_.back().name) + ">");
    }
    return close();
  }

  bool close()
  {
    Open o = std::move(open_.back());
    open_.pop_back();
    if (!o.hasText) {
      return true;
    }
    DraftNode &n = nodes_[o.node];
    DraftAttr a;
    a.id = n.kind == LayoutKind::Custom ? LayoutAttrId::Text : LayoutAttrId::Label;
    a.value = n.kind == LayoutKind::Custom ? trimmed(o.text) : collapsed(o.text);
    a.offset = a.valueOffset = o.textOffset;
    if (n.find(a.id)) {
      return fail(o.textOffset, "there is a label and text");
    }
    n.attrs.push_back(std::move(a));
    return true;
  }

  /* Up to the next '<'. */
  bool text()
  {
    Open *o = open_.empty() ? nullptr : &open_.back();
    while (at_ < s_.size() && s_[at_] != '<') {
      const char c = s_[at_];
      if (!isSpace(c)) {
        if (!o) {
          return fail(at_, "there is text outside of the <layout>");
        }
        if (!hasText(nodes_[o->node].kind)) {
          return fail(at_, "a <" + std::string(o->name) +
                               "> can't hold text: write it in a <label>");
        }
        if (!o->hasText) {
          o->hasText = true;
          o->textOffset = at_;
        }
      }
      std::string decoded;
      if (c == '&') {
        if (!entity(decoded)) {
          return false;
        }
      }
      else if (starts("]]>")) {
        return fail(at_, "']]>' in text is written ]]&gt;");
      }
      else {
        at_ += c == '\r' && at_ + 1 < s_.size() && s_[at_ + 1] == '\n' ? 2 : 1;
        decoded = c == '\r' ? '\n' : c;
      }
      if (o) {
        o->text += decoded;
      }
    }
    return true;
  }

  std::string_view s_;
  size_t at_ = 0;
  std::vector<DraftNode> nodes_;
  std::vector<Open> open_;
};

} // namespace

LayoutPosition layoutPosition(std::string_view xml, size_t offset, const LayoutPosition &from)
{
  LayoutPosition out = from;
  out.offset = offset < xml.size() ? offset : xml.size();
  const size_t start = xml.substr(0, kBom.size()) == kBom ? kBom.size() : 0;
  for (size_t i = from.offset > start ? from.offset : start; i < out.offset; i++) {
    const char c = xml[i];
    if (c == '\n' || (c == '\r' && !(i + 1 < xml.size() && xml[i + 1] == '\n'))) {
      out.line++;
      out.column = 1;
    }
    else if ((uint8_t(c) & 0xC0) != 0x80) {
      out.column++;
    }
  }
  return out;
}

Result<Layout> parseLayout(std::string_view xml, LayoutPosition *where)
{
  Parser parser(xml);
  std::vector<DraftNode> nodes;
  DraftFault fault;
  if (parser.run(nodes)) {
    Result<Layout> out = LayoutAssembler::assemble(std::move(nodes), fault);
    if (out.ok()) {
      return out;
    }
  }
  else {
    fault.status = std::move(parser.error);
    fault.offset = parser.offset;
  }
  const LayoutPosition p = layoutPosition(xml, fault.offset);
  if (where) {
    *where = p;
  }
  Status s = Status::error(fault.status.code(),
                           ("layout " + std::to_string(p.line) + ":" + std::to_string(p.column) +
                            ": " + fault.status.message())
                               .c_str());
  for (const litestl::util::string &suggestion : fault.status.suggestions()) {
    s.suggest(suggestion);
  }
  return s;
}

} // namespace coredcc
