#include "coredcc/scene/outline.h"

#include <cmath>

namespace coredcc::detail {

namespace {

/* A projected point. */
struct Corner {
  double x = 0.0, y = 0.0;
  double w = 0.0;
  double3 q;
};

double cross(const Corner &a, const Corner &b)
{
  return a.x * b.y - a.y * b.x;
}

/* The point of a simplex nearest to the origin, as weights of some of its corners. */
struct Nearest {
  int count = 0;
  int at[3] = {0, 0, 0};
  double weight[3] = {1.0, 0.0, 0.0};
  double x = 0.0, y = 0.0;
  double distance = 0.0;
};

Nearest nearestOnPoint(const Corner *c, int a)
{
  Nearest r;
  r.count = 1;
  r.at[0] = a;
  r.x = c[a].x;
  r.y = c[a].y;
  r.distance = std::hypot(r.x, r.y);
  return r;
}

Nearest nearestOnSegment(const Corner *c, int a, int b)
{
  const double dx = c[b].x - c[a].x, dy = c[b].y - c[a].y, dd = dx * dx + dy * dy;
  const double t = dd > 0.0 ? -(c[a].x * dx + c[a].y * dy) / dd : 0.0;
  if (!(t > 0.0)) {
    return nearestOnPoint(c, a);
  }
  if (t >= 1.0) {
    return nearestOnPoint(c, b);
  }
  /* The foot of the perpendicular, from the cross product: `a + t d` would cancel. */
  const double k = (dx * c[a].y - dy * c[a].x) / dd;
  Nearest r;
  r.count = 2;
  r.at[0] = a;
  r.at[1] = b;
  r.weight[0] = 1.0 - t;
  r.weight[1] = t;
  r.x = -dy * k;
  r.y = dx * k;
  r.distance = std::hypot(r.x, r.y);
  return r;
}

Nearest nearestOnTriangle(const Corner *c, int a, int b, int d)
{
  Nearest best = nearestOnSegment(c, a, b);
  for (const Nearest &n : {nearestOnSegment(c, b, d), nearestOnSegment(c, d, a)}) {
    best = n.distance < best.distance ? n : best;
  }
  const double u = cross(c[b], c[d]), v = cross(c[d], c[a]), w = cross(c[a], c[b]);
  const double sum = u + v + w;
  if (sum != 0.0 &&
      ((u >= 0.0 && v >= 0.0 && w >= 0.0) || (u <= 0.0 && v <= 0.0 && w <= 0.0)))
  {
    Nearest r;
    r.count = 3;
    r.at[0] = a;
    r.at[1] = b;
    r.at[2] = d;
    r.weight[0] = u / sum;
    r.weight[1] = v / sum;
    r.weight[2] = w / sum;
    /* At the origin, unless the triangle has no area and these are noise. */
    for (int i = 0; i < 3; i++) {
      r.x += r.weight[i] * c[r.at[i]].x;
      r.y += r.weight[i] * c[r.at[i]].y;
    }
    r.distance = std::hypot(r.x, r.y);
    best = r.distance < best.distance ? r : best;
  }
  return best;
}

/* In the object's space: weights on the screen are divided by w. */
double3 unprojected(const Corner *c, const Nearest &n)
{
  double3 sum;
  double total = 0.0;
  for (int i = 0; i < n.count; i++) {
    const Corner &p = c[n.at[i]];
    const double k = n.weight[i] / p.w;
    sum = sum + p.q * k;
    total += k;
  }
  return sum / total;
}

/* ---- capsules ---- */

double3 xyz(const double4 &v)
{
  return double3(v[0], v[1], v[2]);
}

/* p, moved by r in the direction of v. */
double3 toward(const double3 &p, const double3 &v, double r)
{
  const double l = std::hypot(v[0], v[1], v[2]);
  return l > 0.0 ? p + v * (r / l) : p;
}

/* A capsule, where `m.q + d >= 0`. */
struct CutCapsule {
  double3 a, b;
  double r;
  double3 m;
  double d;

  double side(const double3 &q) const
  {
    return m.dot(q) + d;
  }

  /* The point with the largest `g.q`: the best of each end's ball cut by the plane, and
   * of where the plane crosses between them. There, the normal is `g + k m` with k >= 0,
   * across the axis. False if nothing of the capsule is left. */
  bool support(const double3 &g, double3 &best) const
  {
    bool any = false;
    double value = 0.0;
    const auto take = [&](const double3 &q) {
      const double v = g.dot(q);
      if (!any || v > value) {
        any = true;
        value = v;
        best = q;
      }
    };
    const double mm = m.dot(m), length = std::sqrt(mm);
    /* Along the plane. What is left of a g across it is noise, which may point anywhere.
     */
    double3 along = g - m * (g.dot(m) / mm);
    along = along - m * (along.dot(m) / mm);
    if (!(along.dot(along) > 1e-24 * g.dot(g))) {
      along = double3();
    }
    for (const double3 &c : {a, b}) {
      const double3 q = toward(c, g, r);
      if (side(q) >= 0.0) {
        take(q);
        continue;
      }
      const double distance = side(c) / length;
      if (!(distance >= -r)) {
        continue;
      }
      const double3 center = c - m * (distance / length);
      const double radius = std::sqrt(std::fmax(0.0, r * r - distance * distance));
      take(toward(center, along, radius));
    }
    const double3 axis = b - a;
    const double slope = m.dot(axis);
    if (slope != 0.0) {
      const double k = -g.dot(axis) / slope;
      if (k >= 0.0) {
        const double3 normal = g + m * k;
        const double3 qa = toward(a, normal, r), qb = toward(b, normal, r);
        const double fa = side(qa), fb = side(qb);
        if ((fa < 0.0) != (fb < 0.0)) {
          take(qa + (qb - qa) * (fa / (fa - fb)));
        }
      }
    }
    return any;
  }
};

/* A capsule between the near and the far plane. */
struct ClippedCapsule {
  CutCapsule front, back;

  ClippedCapsule(const ScreenMap &map, const double3 &a, const double3 &b, double r)
      : front{a, b, r, xyz(map.front), map.front[3]},
        back{a, b, r, xyz(map.back), map.back[3]}
  {
  }

  /* The largest point before one of the planes is the answer if it is also before the
   * other. If neither is, both values are the same, since the planes are parallel and
   * something is between them: so is anything on the line between the two points. */
  bool support(const double3 &g, double3 &best) const
  {
    if (!front.support(g, best)) {
      return false;
    }
    const double past = back.side(best);
    if (past >= 0.0) {
      return true;
    }
    double3 other;
    if (!back.support(g, other)) {
      return false;
    }
    const double before = front.side(other);
    if (before >= 0.0) {
      best = other;
      return true;
    }
    /* In the middle of what of that line is between the planes. */
    const double in = front.side(best), over = back.side(other);
    const double t0 = past / (past - over), t1 = in / (in - before);
    best = best + (other - best) * (0.5 * (t0 + t1));
    return true;
  }
};

bool project(const ScreenMap &map, const double3 &q, Corner &out)
{
  out.q = q;
  out.w = dotPoint(map.w, q);
  if (!(out.w > 0.0)) {
    return false; /* beyond double's precision */
  }
  out.x = dotPoint(map.x, q) / out.w;
  out.y = dotPoint(map.y, q) / out.w;
  return bvh::finite(out.x) && bvh::finite(out.y);
}

/* The point of the solid that is farthest in the direction (nx, ny) on the screen. `at`
 * is any of its points, and becomes that one. For the value h of a point, the one with
 * the largest `n.(x, y) - h w` has a larger value, until there is none. */
bool screenSupport(
    const ScreenMap &map, const ClippedCapsule &solid, double nx, double ny, Corner &at)
{
  double h = nx * at.x + ny * at.y;
  const double3 across = xyz(map.x) * nx + xyz(map.y) * ny, w = xyz(map.w);
  for (int i = 0; i < 20; i++) {
    double3 q;
    Corner c;
    if (!solid.support(across - w * h, q) || !project(map, q, c)) {
      return false;
    }
    const double value = nx * c.x + ny * c.y;
    if (!(value > h + 1e-13 * (1.0 + std::fabs(h)))) {
      break;
    }
    h = value;
    at = c;
  }
  return true;
}

} // namespace

/* GJK: the simplex's nearest point approaches the outline's. */
bool closestOnCapsule(const ScreenMap &map,
                      const double3 &a,
                      const double3 &b,
                      double r,
                      double limit,
                      ScreenPoint &out)
{
  const ClippedCapsule solid(map, a, b, r);
  double3 first;
  Corner c[3];
  if (!solid.support(solid.front.m, first) || !project(map, first, c[0])) {
    return false;
  }
  Nearest n = nearestOnPoint(c, 0);
  Corner at = c[0];
  for (int i = 0; i < 64 && n.count < 3 && n.distance > kCoveredPixels; i++) {
    const double nx = -n.x / n.distance, ny = -n.y / n.distance;
    if (!screenSupport(map, solid, nx, ny, at)) {
      return false;
    }
    /* Everything is at least this far away. */
    const double atLeast = -(nx * at.x + ny * at.y);
    if (atLeast > std::fmax(limit, kCoveredPixels)) {
      return false;
    }
    if (atLeast > 0.0 && n.distance - atLeast < 0.01) {
      break;
    }
    /* The corners of the nearest point, and the new one. */
    Corner kept[3];
    for (int k = 0; k < n.count; k++) {
      kept[k] = c[n.at[k]];
    }
    kept[n.count] = at;
    const Nearest next =
        n.count == 1 ? nearestOnSegment(kept, 0, 1) : nearestOnTriangle(kept, 0, 1, 2);
    if (!(next.distance < n.distance)) {
      break;
    }
    for (int k = 0; k <= n.count; k++) {
      c[k] = kept[k];
    }
    n = next;
  }
  out.distance = n.distance > kCoveredPixels ? n.distance : 0.0;
  out.point = unprojected(c, n);
  return true;
}

/* ---- corners ---- */

namespace {

/* Not divided yet. `side` is front or back: what is cut next. */
struct Homogeneous {
  double x, y, w;
  double side[2];
  double3 q;
};

/* From the visible end, whichever triangle the edge is of, so that shared edges stay
 * shared. */
Homogeneous onPlane(const Homogeneous &in, const Homogeneous &outside, int plane)
{
  const double t = in.side[plane] / (in.side[plane] - outside.side[plane]);
  const auto mix = [t](double a, double b) { return a + (b - a) * t; };
  Homogeneous r{mix(in.x, outside.x),
                mix(in.y, outside.y),
                mix(in.w, outside.w),
                {mix(in.side[0], outside.side[0]), mix(in.side[1], outside.side[1])},
                in.q + (outside.q - in.q) * t};
  r.side[plane] = 0.0;
  return r;
}

/* Cuts a point, a segment or a closed polygon. Returns how many corners there are now. */
int cut(const Homogeneous *from, int count, int plane, Homogeneous *to)
{
  int n = 0;
  for (int i = 0; i < count; i++) {
    const Homogeneous &p = from[i], &next = from[(i + 1) % count];
    const bool in = p.side[plane] >= 0.0, nextIn = next.side[plane] >= 0.0;
    if (in) {
      to[n++] = p;
    }
    /* A segment has one edge. */
    if (in != nextIn && (count > 2 || i == 0)) {
      to[n++] = in ? onPlane(p, next, plane) : onPlane(next, p, plane);
    }
  }
  return n;
}

} // namespace

bool closestOnCorners(const ScreenMap &map,
                      const double3 *corners,
                      int count,
                      ScreenPoint &out)
{
  Homogeneous h[5], cutOnce[5];
  for (int i = 0; i < count; i++) {
    const double3 &q = corners[i];
    h[i] = {dotPoint(map.x, q),
            dotPoint(map.y, q),
            dotPoint(map.w, q),
            {dotPoint(map.front, q), dotPoint(map.back, q)},
            q};
  }
  /* A triangle that has been cut to a point or a line is as good as one. */
  int n = cut(h, count, 0, cutOnce);
  n = cut(cutOnce, n, 1, h);
  Corner c[5];
  for (int i = 0; i < n; i++) {
    c[i] = {h[i].x / h[i].w, h[i].y / h[i].w, h[i].w, h[i].q};
    if (!(h[i].w > 0.0) || !bvh::finite(c[i].x) || !bvh::finite(c[i].y)) {
      return false;
    }
  }
  if (n == 0) {
    return false;
  }
  Nearest best = n == 1 ? nearestOnPoint(c, 0) : nearestOnSegment(c, 0, 1);
  for (int i = 2; i < n; i++) {
    const Nearest other = nearestOnTriangle(c, 0, i - 1, i);
    best = i == 2 || other.distance < best.distance ? other : best;
  }
  out.distance = best.distance > kCoveredPixels ? best.distance : 0.0;
  out.point = unprojected(c, best);
  return true;
}

} // namespace coredcc::detail
