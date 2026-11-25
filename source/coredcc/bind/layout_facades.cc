#include "coredcc/bind/layout_facades.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/layout/validate.h"
#include "coredcc/layout/xml.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace coredcc::script {

using detail::failed;
using detail::ok;
using detail::view;

/* Where the issues are, found in one pass over the text. */
static std::vector<LayoutPosition> positions(std::string_view xml,
                                             const litestl::util::Vector<LayoutIssue> &issues)
{
  std::vector<int> order(size_t(issues.size()));
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return issues[a].offset < issues[b].offset; });
  std::vector<LayoutPosition> out(order.size());
  LayoutPosition at;
  for (int i : order) {
    at = layoutPosition(xml, issues[i].offset, at);
    out[size_t(i)] = at;
  }
  return out;
}

StringResult layoutJson(const string &xml)
{
  Result<Layout> layout = parseLayout(view(xml));
  return layout.ok() ? ok<StringResult>(string(layoutToJson(*layout).c_str())) :
                       failed<StringResult>(layout.status());
}

StringResult validateLayoutXml(Context &ctx, const string &xml)
{
  if (Status s = detail::kernelGate(ctx, "validateLayoutXml"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<Layout> layout = parseLayout(view(xml));
  if (!layout.ok()) {
    return failed<StringResult>(layout.status());
  }
  const litestl::util::Vector<LayoutIssue> issues =
      validateLayout(ctx.runtime(), ctx.root().type, *layout);
  const std::vector<LayoutPosition> at = positions(view(xml), issues);
  JsonWriter out;
  out.beginArray();
  for (size_t k = 0; k < at.size(); k++) {
    const LayoutIssue &i = issues[int(k)];
    out.beginObject();
    out.key("severity").string(i.severity == LayoutSeverity::Error ? "Error" : "Warning");
    out.key("line").integer(at[k].line);
    out.key("column").integer(at[k].column);
    out.key("node").integer(i.node);
    out.key("code").string(statusCodeName(i.status.code()));
    out.key("message").string(i.status.message());
    if (i.status.suggestions().size() > 0) {
      out.key("suggestions").beginArray();
      for (const string &s : i.status.suggestions()) {
        out.string(s.c_str());
      }
      out.endArray();
    }
    out.endObject();
  }
  out.endArray();
  return ok<StringResult>(string(out.take().c_str()));
}

BagResult invokeAt(Context &ctx, const string &prefix, const string &invocation)
{
  if (Status s = detail::slotGate(ctx, "invokeAt", {"exec"}); !s.ok()) {
    return failed<BagResult>(std::move(s));
  }
  if (prefix.size() == 0) {
    PrefixScope root(ctx, nullptr);
    return invoke(ctx, invocation);
  }
  Result<Path> path = parsePath(view(prefix));
  if (!path.ok()) {
    return failed<BagResult>(path.status());
  }
  PrefixScope scope(ctx, &*path);
  if (Status s = resolve(ctx, Path()).status(); !s.ok()) {
    return failed<BagResult>(std::move(s));
  }
  return invoke(ctx, invocation);
}

} // namespace coredcc::script
