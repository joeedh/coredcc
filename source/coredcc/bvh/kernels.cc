/* The watertight triangle test needs a*b - c*d to change sign exactly when the products
 * swap, which a fused multiply-add breaks. These pragmas (and the flags CMake sets for
 * this file) keep every product rounded, whatever the host's own flags are. */
#if defined(__clang__)
#if !defined(__wasm__) /* not supported there */
#pragma float_control(precise, on)
#endif
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma float_control(precise, on)
#pragma fp_contract(off)
#endif

#include "coredcc/bvh/kernels.h"

#include <limits>

namespace coredcc::bvh {

namespace {

double dot(const double3 &a, const double3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double distanceSq(const double3 &a, const double3 &b)
{
  const double3 d = a - b;
  return dot(d, d);
}

/* Where a ray enters a circle or sphere of radius r. It comes nearest to the center at
 * `mid`, `missSq` (squared) away; `speedSq` is the ray's squared speed. */
bool entry(double mid, double missSq, double speedSq, double r, double &t)
{
  const double h = r * r - missSq;
  if (!(h >= 0.0)) {
    return false;
  }
  t = mid - std::sqrt(h / speedSq);
  return true;
}

} // namespace

template <class S>
uint32_t rayTriangles(const RayQuery &q,
                      const S *const coords[9],
                      int first,
                      int count,
                      double limit,
                      TriangleBlockHits &out)
{
  /* Corner c's ray-space x, y and z. */
  double x[3][kTriangleBlock], y[3][kTriangleBlock], z[3][kTriangleBlock];
  const double ox = q.origin[q.kx], oy = q.origin[q.ky], oz = q.origin[q.kz];
  const double sx = q.shearX, sy = q.shearY, sz = q.shearZ;
  for (int c = 0; c < 3; c++) {
    const S *px = coords[c * 3 + q.kx] + first;
    const S *py = coords[c * 3 + q.ky] + first;
    const S *pz = coords[c * 3 + q.kz] + first;
    for (int k = 0; k < count; k++) {
      const double dz = double(pz[k]) - oz;
      x[c][k] = (double(px[k]) - ox) - sx * dz;
      y[c][k] = (double(py[k]) - oy) - sy * dz;
      z[c][k] = sz * dz;
    }
  }
  double eu[kTriangleBlock], ev[kTriangleBlock], ew[kTriangleBlock], det[kTriangleBlock];
  for (int k = 0; k < count; k++) {
    eu[k] = x[2][k] * y[1][k] - y[2][k] * x[1][k];
    ev[k] = x[0][k] * y[2][k] - y[0][k] * x[2][k];
    ew[k] = x[1][k] * y[0][k] - y[1][k] * x[0][k];
    det[k] = eu[k] + ev[k] + ew[k];
    out.t[k] = eu[k] * z[0][k] + ev[k] * z[1][k] + ew[k] * z[2][k];
  }
  uint32_t hits = 0;
  for (int k = 0; k < count; k++) {
    /* Closed signs: an edge function of 0 counts as inside on both sides of the edge. */
    const bool neg = eu[k] < 0.0 || ev[k] < 0.0 || ew[k] < 0.0;
    const bool pos = eu[k] > 0.0 || ev[k] > 0.0 || ew[k] > 0.0;
    if ((neg && pos) || det[k] == 0.0) {
      continue;
    }
    double lo[3], hi[3];
    for (int axis = 0; axis < 3; axis++) {
      const double a = double(coords[axis][first + k]), b = double(coords[3 + axis][first + k]);
      const double c = double(coords[6 + axis][first + k]);
      lo[axis] = std::fmin(a, std::fmin(b, c));
      hi[axis] = std::fmax(a, std::fmax(b, c));
    }
    double t = out.t[k] / det[k];
    if (q.keep(lo, hi, t) && t >= q.tMin && t <= limit) {
      out.t[k] = t;
      out.u[k] = ev[k] / det[k];
      out.v[k] = ew[k] / det[k];
      /* dir . ((b - a) x (c - a)) is -det * dir[kz] (the axes are a right-handed cycle). */
      out.back[k] = (det[k] < 0.0) == (q.dir[q.kz] > 0.0);
      hits |= 1u << k;
    }
  }
  return hits;
}

template uint32_t rayTriangles<float>(
    const RayQuery &, const float *const[9], int, int, double, TriangleBlockHits &);
template uint32_t rayTriangles<double>(
    const RayQuery &, const double *const[9], int, int, double, TriangleBlockHits &);

/* The ray is taken relative to the solid: a point of it, in the world's coordinates, has their
 * rounding. */
static bool sphere(
    const RayQuery &q, const double3 &c, double r, double limit, double &t, bool &inside)
{
  const double3 from = q.origin - c;
  const double3 start = from + q.dir * q.tMin;
  inside = dot(start, start) <= r * r;
  if (inside) {
    t = q.tMin;
    return q.tMin <= limit;
  }
  /* From the closest approach, not b^2 - ac: that cancels for far, small spheres. */
  const double speedSq = dot(q.dir, q.dir);
  const double mid = -dot(from, q.dir) / speedSq;
  const double3 miss = from + q.dir * mid;
  return entry(mid, dot(miss, miss), speedSq, r, t) && t >= q.tMin && t <= limit;
}

bool raySphere(
    const RayQuery &q, const double3 &c, double r, double limit, double &t, bool &inside)
{
  return r >= 0.0 && sphere(q, c, r, limit, t, inside);
}

bool rayCapsule(const RayQuery &q,
                const double3 &a,
                const double3 &b,
                double r,
                double limit,
                double &t,
                bool &inside)
{
  if (!(r >= 0.0)) {
    return false;
  }
  const double3 from = q.origin - a, axis = b - a;
  const double3 start = from + q.dir * q.tMin;
  inside = distanceSq(start, closestOnSegment(start, double3(), axis)) <= r * r;
  if (inside) {
    t = q.tMin;
    return q.tMin <= limit;
  }
  /* From outside, the first entry into any of the three parts. */
  bool hit = false;
  t = limit;
  double s;
  bool in;
  if (sphere(q, a, r, t, s, in)) {
    t = s;
    hit = true;
  }
  if (sphere(q, b, r, t, s, in)) {
    t = s;
    hit = true;
  }
  const double len = std::sqrt(dot(axis, axis));
  if (!(len > 0.0) || !finite(len)) {
    return hit;
  }
  /* The side: the circle problem across the axis, for hits between the ends. */
  const double3 n = axis * (1.0 / len);
  const double3 across = q.dir - n * dot(q.dir, n);
  const double speedSq = dot(across, across);
  if (speedSq > 0.0) {
    const double mid = -dot(from, across) / speedSq;
    double3 miss = from + q.dir * mid;
    miss = miss - n * dot(miss, n);
    if (entry(mid, dot(miss, miss), speedSq, r, s)) {
      const double along = dot(from + q.dir * s, n);
      if (along >= 0.0 && along <= len && s >= q.tMin && s <= t) {
        t = s;
        hit = true;
      }
    }
  }
  return hit;
}

bool rayBox(const RayQuery &q, const Box3d &box, double limit, double &t, bool &inside)
{
  inside = box.contains(q.at(q.tMin));
  return q.boxSlab(box.min, box.max, limit, t);
}

double3 closestOnSegment(const double3 &p, const double3 &a, const double3 &b)
{
  const double3 ab = b - a;
  const double len = dot(ab, ab);
  if (!(len > 0.0)) {
    return a;
  }
  const double s = dot(p - a, ab) / len;
  return s <= 0.0 ? a : s >= 1.0 ? b : a + ab * s;
}

double3
closestOnTriangle(const double3 &p, const double3 &a, const double3 &b, const double3 &c)
{
  const double3 ab = b - a, ac = c - a, n = ab.cross(ac);
  const double nn = dot(n, n), sides = dot(ab, ab) * dot(ac, ac);
  double3 best;
  double bestD = (std::numeric_limits<double>::infinity)();
  if (nn > 0.0) {
    /* Inside the prism over the triangle: the projection onto its plane. A sliver's normal may be
     * noise; the point is still one of the triangle's, but an edge may be nearer. */
    const double3 ap = p - a;
    const double v = dot(ap.cross(ac), n), w = dot(ab.cross(ap), n);
    if (v >= 0.0 && w >= 0.0 && v + w <= nn) {
      best = a + ab * (v / nn) + ac * (w / nn);
      if (nn > 1e-8 * sides) {
        return best;
      }
      bestD = distanceSq(p, best);
    }
  }
  for (const double3 &e :
       {closestOnSegment(p, a, b), closestOnSegment(p, b, c), closestOnSegment(p, c, a)})
  {
    const double d = distanceSq(p, e);
    if (d < bestD) {
      best = e;
      bestD = d;
    }
  }
  return best;
}

double3 closestOnBox(const double3 &p, const Box3d &box)
{
  double3 r = p;
  for (int i = 0; i < 3; i++) {
    r[i] = r[i] < box.min[i] ? box.min[i] : r[i] > box.max[i] ? box.max[i] : r[i];
  }
  return r;
}

double3 closestOnSphere(const double3 &p, const double3 &c, double r)
{
  const double3 d = p - c;
  const double len = std::sqrt(dot(d, d));
  return len <= r ? p : c + d * (r / len);
}

} // namespace coredcc::bvh
