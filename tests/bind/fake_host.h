#pragma once

/* A fake foreign host for the bind tests: its "JS objects" are maps of named C++
 * functions, looked up by handle. Each handle carries the one count C++ holds; release
 * drops the object and records the release, so double releases show. */

#include "litestl/binding/binding.h"
#include "litestl/binding/foreign.h"

#include <functional>
#include <map>
#include <string>

namespace fakehost {

namespace lf = litestl::binding::foreign;
using litestl::util::string;

/** A method: `args` and `ret` use the thunk ABI. Return false and set `err` to throw. */
using Fn = std::function<bool(void **args, void *ret, string *err)>;

struct Obj {
  std::map<std::string, Fn> methods;
};

class Host {
public:
  Host()
  {
    host_ = {&Host::callFn, &Host::releaseFn, this};
    lf::setHost(&host_);
  }
  ~Host()
  {
    lf::setHost(nullptr);
  }
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;

  lf::Handle add(Obj o)
  {
    const lf::Handle h = next_++;
    objs_[h] = std::move(o);
    return h;
  }
  int live() const
  {
    return int(objs_.size());
  }
  int releasesOf(lf::Handle h) const
  {
    auto it = releases_.find(h);
    return it == releases_.end() ? 0 : it->second;
  }
  /** Every handle issued so far was released exactly once. */
  bool allReleasedOnce() const
  {
    for (lf::Handle h = 1; h < next_; h++) {
      if (releasesOf(h) != 1) {
        return false;
      }
    }
    return releasesOf(0) == 0;
  }
  int calls = 0;

private:
  static bool callFn(void *ctx,
                     lf::Handle h,
                     const litestl::binding::types::Method *m,
                     void **args,
                     void *ret,
                     string *err)
  {
    Host &self = *static_cast<Host *>(ctx);
    self.calls++;
    auto it = self.objs_.find(h);
    if (it == self.objs_.end()) {
      *err = string(("handle " + std::to_string(h) + " is not live").c_str());
      return false;
    }
    auto f = it->second.methods.find(m->getJsName().c_str());
    if (f == it->second.methods.end()) {
      *err = string((std::string(m->getJsName().c_str()) + " is not a function").c_str());
      return false;
    }
    Fn fn = f->second; /* the call may release this object */
    return fn(args, ret, err);
  }
  static void releaseFn(void *ctx, lf::Handle h)
  {
    Host &self = *static_cast<Host *>(ctx);
    self.releases_[h]++;
    self.objs_.erase(h);
  }

  lf::Host host_{};
  std::map<lf::Handle, Obj> objs_;
  std::map<lf::Handle, int> releases_;
  lf::Handle next_ = 1;
};

} // namespace fakehost
