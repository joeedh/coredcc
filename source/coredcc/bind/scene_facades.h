#pragma once

/** \file
 * Scenes for scripts. The rules of facades.h hold here too. Blocks are pointers as text
 * (api/ref_text.h). Everything that takes a Context is Busy inside a script slot other than a
 * command's `exec`.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/draw/view.h"
#include "coredcc/scene/pick.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/vector.h"

namespace coredcc::script {

/* ---- double3 ---- */

double3 vec3(double x, double y, double z);
/** 0 out of range. */
double vec3At(const double3 &v, int32_t i);
/** Nothing out of range. */
void vec3Put(double3 &v, int32_t i, double x);

/* ---- scenes ---- */

/** Makes a scene, undoably. Its value is a pointer to it. It doesn't become the Context's. */
ValueResult newScene(Context &ctx, const string &name);
/** The scene that commands and the functions below work on; "" for none. Not undoable. */
Status setScene(Context &ctx, const string &scene);
/** A block's id as the `objects` input of commands takes it. */
StringResult blockIdText(Context &ctx, const string &block);
/** coredcc::evaluateScene for the Context's scene. */
Status evaluateScene(Context &ctx);

/** Results of the last evaluation as flat arrays, for drawing. One row for each place in
 * Scene::evaluatedObjects that was asked for. */
struct SceneTable {
  using Doubles = litestl::util::Vector<double>;
  using Ints = litestl::util::Vector<int32_t>;

  /* 16 for each, column by column: ObjectEval::dataWorld. */
  Doubles matrixList;
  /* Minimum and maximum, 6 for each, in the world. An empty box has a minimum above its maximum. */
  Doubles boundsList;
  /* EvalStatus, ObjectFlags, the parent's place or -1, and whether it is a member. */
  Ints statusList, flagsList, parentList, memberList;
  /* Scene::lastChanges. */
  Ints changedList;
  double changeVersion = 0.0;
  bool changedAll = false;
  /* How many places there are. */
  int32_t placeCount = 0;

  Doubles matrices() const
  {
    return matrixList;
  }
  Doubles bounds() const
  {
    return boundsList;
  }
  Ints status() const
  {
    return statusList;
  }
  Ints flags() const
  {
    return flagsList;
  }
  Ints parents() const
  {
    return parentList;
  }
  Ints members() const
  {
    return memberList;
  }
  Ints changed() const
  {
    return changedList;
  }
  double version() const
  {
    return changeVersion;
  }
  bool allChanged() const
  {
    return changedAll;
  }
  int32_t places() const
  {
    return placeCount;
  }
  static const litestl::binding::types::Struct<SceneTable> *defineBindings();
};

struct SceneTableResult {
  Status status;
  SceneTable value;
  static const litestl::binding::types::Struct<SceneTableResult> *defineBindings();
};

/** The results at these places (OutOfRange if one isn't there), or `all` of them. Before the first
 * evaluation there are none. */
SceneTableResult sceneResults(Context &ctx,
                              const litestl::util::Vector<int32_t> &places,
                              bool all);

/** Pointers to blocks, as text. */
struct RefList {
  litestl::util::Vector<string> refs;

  int32_t size() const
  {
    return int32_t(refs.size());
  }
  /** "" out of range. */
  string at(int32_t i) const
  {
    return i >= 0 && i < size() ? refs[i] : string();
  }
  static const litestl::binding::types::Struct<RefList> *defineBindings();
};

struct RefListResult {
  Status status;
  RefList value;
  static const litestl::binding::types::Struct<RefListResult> *defineBindings();
};

/** The objects at these places, or at `all`. "" for one whose record is gone. */
RefListResult sceneRefs(Context &ctx, const litestl::util::Vector<int32_t> &places, bool all);
/** Scene::children; `object` "" for the roots. */
RefListResult sceneChildren(Context &ctx, const string &object);

struct MatrixResult {
  Status status;
  mat4d value;

  MatrixResult()
  {
    value.identity();
  }
  static const litestl::binding::types::Struct<MatrixResult> *defineBindings();
};

/** Where an object is now, at the scene's time (computeState). */
MatrixResult worldMatrix(Context &ctx, const string &object);

/* ---- picking ---- */

/** A PickHit. */
struct PickedObject {
  /** A pointer to it, as text. */
  string object;
  int32_t domain = -1;
  int32_t element = -1;
  double distance = 0.0;
  double depth = 0.0;
  double3 point;
  int32_t instance = -1;
  double3 normal;
  double u = 0.0, v = 0.0;
  bool backFacing = false;
  bool thin = false;

  static const litestl::binding::types::Struct<PickedObject> *defineBindings();
};

struct PickList {
  litestl::util::Vector<PickedObject> hits;

  int32_t size() const
  {
    return int32_t(hits.size());
  }
  /** An empty one out of range. */
  PickedObject at(int32_t i) const
  {
    return i >= 0 && i < size() ? hits[i] : PickedObject();
  }
  static const litestl::binding::types::Struct<PickList> *defineBindings();
};

struct PickResult {
  Status status;
  PickList value;
  static const litestl::binding::types::Struct<PickResult> *defineBindings();
};

/**
 * Evaluates the Context's scene, and gives the objects near a pixel (pickScene), without hidden
 * and unselectable ones. A script's exception comes with the hits of the other objects. When the
 * scene can't be evaluated, there are none.
 */
PickResult pick(Context &ctx, const View &view, double x, double y, const PickOptions &options);

} // namespace coredcc::script
