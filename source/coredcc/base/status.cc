#include "coredcc/base/status.h"

#include "litestl/util/alloc.h"

namespace coredcc {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

struct Status::Rep {
  StatusCode code;
  string message;
  Vector<string> suggestions;
};

const char *statusCodeName(StatusCode code)
{
  switch (code) {
    case StatusCode::Ok:
      return "Ok";
    case StatusCode::InvalidArgument:
      return "InvalidArgument";
    case StatusCode::NotFound:
      return "NotFound";
    case StatusCode::AlreadyExists:
      return "AlreadyExists";
    case StatusCode::TypeMismatch:
      return "TypeMismatch";
    case StatusCode::OutOfRange:
      return "OutOfRange";
    case StatusCode::FailedPrecondition:
      return "FailedPrecondition";
    case StatusCode::Busy:
      return "Busy";
    case StatusCode::Cancelled:
      return "Cancelled";
    case StatusCode::Unsupported:
      return "Unsupported";
    case StatusCode::Internal:
      return "Internal";
  }
  return "Unknown";
}

Status Status::error(StatusCode code, stringref message)
{
  CDCC_CHECK(code != StatusCode::Ok, "Status::error needs an error code");
  Status s;
  s.rep_ = litestl::alloc::New<Rep>("coredcc::Status", code, string(message), Vector<string>());
  return s;
}

Status::Status(const Status &b)
{
  if (b.rep_) {
    rep_ = litestl::alloc::New<Rep>("coredcc::Status", *b.rep_);
  }
}

Status &Status::operator=(const Status &b)
{
  if (this != &b) {
    Status copy(b);
    *this = std::move(copy);
  }
  return *this;
}

Status &Status::operator=(Status &&b) noexcept
{
  if (this != &b) {
    if (rep_) {
      litestl::alloc::Delete(rep_);
    }
    rep_ = std::exchange(b.rep_, nullptr);
  }
  return *this;
}

Status::~Status()
{
  if (rep_) {
    litestl::alloc::Delete(rep_);
  }
}

StatusCode Status::code() const
{
  return rep_ ? rep_->code : StatusCode::Ok;
}

const char *Status::message() const
{
  return rep_ ? rep_->message.c_str() : "";
}

const Vector<string> &Status::suggestions() const
{
  static const Vector<string> none;
  return rep_ ? rep_->suggestions : none;
}

Status &Status::suggest(stringref suggestion) &
{
  if (rep_) {
    rep_->suggestions.append(string(suggestion));
  }
  return *this;
}

Status &&Status::suggest(stringref suggestion) &&
{
  return std::move(suggest(suggestion));
}

string Status::toString() const
{
  if (!rep_) {
    return string("Ok");
  }
  string out(statusCodeName(rep_->code));
  out += ": ";
  out += rep_->message;
  if (rep_->suggestions.size() > 0) {
    out += " (did you mean: ";
    for (int i = 0; i < int(rep_->suggestions.size()); i++) {
      if (i > 0) {
        out += ", ";
      }
      out += rep_->suggestions[i];
    }
    out += "?)";
  }
  return out;
}

} // namespace coredcc
