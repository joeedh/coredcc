#include "coredcc/graph/exec_context.h"

#include "coredcc/base/assert.h"
#include "coredcc/graph/node_type.h"

#include <string>

namespace coredcc {

NodeExecContext::NodeExecContext(const Setup &setup) : s_(setup)
{
  CDCC_CHECK(s_.rt && s_.type && s_.inputs.size() == size_t(s_.type->inputs.size()) &&
                 s_.params.size() == size_t(s_.type->params.size()) &&
                 s_.params.size() == s_.paramBlocks.size(),
             "NodeExecContext: the setup doesn't match the node type");
  outputs_.resize(size_t(s_.type->outputs.size()));
}

const GraphValue &NodeExecContext::input(int i) const
{
  CDCC_CHECK(i >= 0 && i < inputCount(), "NodeExecContext::input: a bad index");
  return *s_.inputs[size_t(i)].value;
}

const GraphValue *NodeExecContext::input(std::string_view name) const
{
  const int i = s_.type->inputIndex(name);
  return i >= 0 ? s_.inputs[size_t(i)].value : nullptr;
}

bool NodeExecContext::isLinked(int i) const
{
  return i >= 0 && i < inputCount() && s_.inputs[size_t(i)].linked;
}

bool NodeExecContext::isLinked(std::string_view name) const
{
  return isLinked(s_.type->inputIndex(name));
}

const PropValue *NodeExecContext::param(std::string_view name) const
{
  const int i = s_.type->paramIndex(name);
  return i >= 0 ? &s_.params[size_t(i)] : nullptr;
}

BlockUse NodeExecContext::paramBlock(std::string_view name)
{
  const int i = s_.type->paramIndex(name);
  return i >= 0 ? useBlock(s_.paramBlocks[size_t(i)]) : BlockUse();
}

BlockUse NodeExecContext::useBlock(BlockId id)
{
  return s_.blocks && !id.isNull() ? s_.blocks->useBlock(id) : BlockUse();
}

IfaceRef NodeExecContext::queryBlockRaw(BlockHandle h, InterfaceId iface)
{
  return s_.blocks ? s_.blocks->queryBlock(h, iface) : IfaceRef();
}

Status NodeExecContext::setOutput(int i, GraphValue v)
{
  Status s;
  if (i < 0 || i >= int(outputs_.size())) {
    s = Status::error(StatusCode::NotFound, "setOutput: there is no such output");
  }
  else if (const NodeOutputDef &def = s_.type->outputs[i]; !def.type->holds(v)) {
    s = Status::error(StatusCode::TypeMismatch,
                      (std::string("output '") + def.name.c_str() + "' is a " +
                       def.type->name.c_str() + ", but was set to a " +
                       graphValueTypeName(v.type()) + " value")
                          .c_str());
  }
  else {
    outputs_[i] = std::move(v);
    return s;
  }
  if (outputError_.ok()) {
    outputError_ = Status::error(StatusCode::Internal, s.message());
  }
  return s;
}

Status NodeExecContext::setOutput(std::string_view name, GraphValue v)
{
  return setOutput(s_.type->outputIndex(name), std::move(v));
}

Status NodeExecContext::checkOutputs() const
{
  CDCC_TRY(outputError_);
  for (int i = 0; i < int(outputs_.size()); i++) {
    if (outputs_[i].isNone()) {
      return Status::error(StatusCode::Internal,
                           (std::string("the kernel didn't set output '") +
                            s_.type->outputs[i].name.c_str() + "'")
                               .c_str());
    }
  }
  return Status();
}

} // namespace coredcc
