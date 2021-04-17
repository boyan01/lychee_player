//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_BASE_RECT_H_
#define MEDIA_BASE_RECT_H_

#include <string>

#include "size.h"

namespace media {
namespace base {

class Insets {
 public:
  Insets() : top_(0), left_(0), bottom_(0), right_(0) {}
  Insets(int top, int left, int bottom, int right)
      : top_(top),
        left_(left),
        bottom_(bottom),
        right_(right) {}

  ~Insets() = default;

  int top() const { return top_; }
  int left() const { return left_; }
  int bottom() const { return bottom_; }
  int right() const { return right_; }

  // Returns the total width taken up by the insets, which is the sum of the
  // left and right insets.
  int width() const { return left_ + right_; }

  // Returns the total height taken up by the insets, which is the sum of the
  // top and bottom insets.
  int height() const { return top_ + bottom_; }

  Insets Scale(float scale) const {
    return Scale(scale, scale);
  }

  Insets Scale(float x_scale, float y_scale) const {
    return Insets(static_cast<int>(top_ * y_scale),
                  static_cast<int>(left_ * x_scale),
                  static_cast<int>(bottom_ * y_scale),
                  static_cast<int>(right_ * x_scale));
  }

  // Returns true if the insets are empty.
  bool empty() const { return width() == 0 && height() == 0; }

  void Set(int top, int left, int bottom, int right) {
    top_ = top;
    left_ = left;
    bottom_ = bottom;
    right_ = right;
  }

  bool operator==(const Insets &insets) const {
    return top_ == insets.top_ && left_ == insets.left_ &&
        bottom_ == insets.bottom_ && right_ == insets.right_;
  }

  bool operator!=(const Insets &insets) const {
    return !(*this == insets);
  }

  Insets &operator+=(const Insets &insets) {
    top_ += insets.top_;
    left_ += insets.left_;
    bottom_ += insets.bottom_;
    right_ += insets.right_;
    return *this;
  }

  Insets operator-() const {
    return Insets(-top_, -left_, -bottom_, -right_);
  }

  // Returns a string representation of the insets.
  std::string ToString() const;

 private:
  int top_;
  int left_;
  int bottom_;
  int right_;
};

// A point has an x and y coordinate.
template<typename Class, typename Type>
class PointBase {
 public:
  Type x() const { return x_; }
  Type y() const { return y_; }

  void SetPoint(Type x, Type y) {
    x_ = x;
    y_ = y;
  }

  void set_x(Type x) { x_ = x; }
  void set_y(Type y) { y_ = y; }

  void Offset(Type delta_x, Type delta_y) {
    x_ += delta_x;
    y_ += delta_y;
  }

  Class Scale(float scale) const {
    return Scale(scale, scale);
  }

  Class Scale(float x_scale, float y_scale) const {
    return Class(static_cast<Type>(x_ * x_scale),
                 static_cast<Type>(y_ * y_scale));
  }

  Class Add(const Class &other) const {
    const auto *orig = static_cast<const Class *>(this);
    Class copy = *orig;
    copy.Offset(other.x_, other.y_);
    return copy;
  }

  Class Subtract(const Class &other) const {
    const auto *orig = static_cast<const Class *>(this);
    Class copy = *orig;
    copy.Offset(-other.x_, -other.y_);
    return copy;
  }

  Class Middle(const Class &other) const {
    return Class((x_ + other.x_) / 2, (y_ + other.y_) / 2);
  }

  bool operator==(const Class &rhs) const {
    return x_ == rhs.x_ && y_ == rhs.y_;
  }

  bool operator!=(const Class &rhs) const {
    return *this != rhs;
  }

  // A point is less than another point if its y-value is closer
  // to the origin. If the y-values are the same, then point with
  // the x-value closer to the origin is considered less than the
  // other.
  // This comparison is required to use Point in sets, or sorted
  // vectors.
  bool operator<(const Class &rhs) const {
    return (y_ == rhs.y_) ? (x_ < rhs.x_) : (y_ < rhs.y_);
  }

 protected:
  PointBase(Type x, Type y) : x_(x), y_(y) {}
  // Destructor is intentionally made non virtual and protected.
  // Do not make this public.
  ~PointBase() = default;

 private:
  Type x_;
  Type y_;
};

// A point has an x and y coordinate.
class Point : public PointBase<Point, int> {
 public:
  Point();
  Point(int x, int y);

  ~Point() = default;

// Returns a string representation of point.
  std::string ToString() const;
};

template<typename Class,
    typename PointClass,
    typename SizeClass,
    typename InsetsClass,
    typename Type>
class RectBase {
 public:
  Type x() const { return origin_.x(); }
  void set_x(Type x) { origin_.set_x(x); }

  Type y() const { return origin_.y(); }
  void set_y(Type y) { origin_.set_y(y); }

  Type width() const { return size_.width(); }
  void set_width(Type width) { size_.set_width(width); }

  Type height() const { return size_.height(); }
  void set_height(Type height) { size_.set_height(height); }

  const PointClass &origin() const { return origin_; }
  void set_origin(const PointClass &origin) { origin_ = origin; }

  const SizeClass &size() const { return size_; }
  void set_size(const SizeClass &size) { size_ = size; }

  Type right() const { return x() + width(); }
  Type bottom() const { return y() + height(); }

  void SetRect(Type x, Type y, Type width, Type height);

  // Shrink the rectangle by a horizontal and vertical distance on all sides.
  void Inset(Type horizontal, Type vertical) {
    Inset(horizontal, vertical, horizontal, vertical);
  }

  // Shrink the rectangle by the given insets.
  void Inset(const InsetsClass &insets);

  // Shrink the rectangle by the specified amount on each side.
  void Inset(Type left, Type top, Type right, Type bottom);

  // Move the rectangle by a horizontal and vertical distance.
  void Offset(Type horizontal, Type vertical);
  void Offset(const PointClass &point) {
    Offset(point.x(), point.y());
  }

  /// Scales the rectangle by |scale|.
  Class Scale(float scale) const {
    return Scale(scale, scale);
  }

  Class Scale(float x_scale, float y_scale) const {
    return Class(origin_.Scale(x_scale, y_scale),
                 size_.Scale(x_scale, y_scale));
  }

  InsetsClass InsetsFrom(const Class &inner) const {
    return InsetsClass(inner.y() - y(),
                       inner.x() - x(),
                       bottom() - inner.bottom(),
                       right() - inner.right());
  }

  // Returns true if the area of the rectangle is zero.
  bool IsEmpty() const { return size_.IsEmpty(); }

  bool operator==(const Class &other) const;

  bool operator!=(const Class &other) const {
    return *this != other;
  }

  // A rect is less than another rect if its origin is less than
  // the other rect's origin. If the origins are equal, then the
  // shortest rect is less than the other. If the origin and the
  // height are equal, then the narrowest rect is less than.
  // This comparison is required to use Rects in sets, or sorted
  // vectors.
  bool operator<(const Class &other) const;

  // Returns true if the point identified by point_x and point_y falls inside
  // this rectangle.  The point (x, y) is inside the rectangle, but the
  // point (x + width, y + height) is not.
  bool Contains(Type point_x, Type point_y) const;

  // Returns true if the specified point is contained by this rectangle.
  bool Contains(const PointClass &point) const {
    return Contains(point.x(), point.y());
  }

  // Returns true if this rectangle contains the specified rectangle.
  bool Contains(const Class &rect) const;

  // Returns true if this rectangle intersects the specified rectangle.
  bool Intersects(const Class &rect) const;

  // Computes the intersection of this rectangle with the given rectangle.
  Class Intersect(const Class &rect) const;

  // Computes the union of this rectangle with the given rectangle.  The union
  // is the smallest rectangle containing both rectangles.
  Class Union(const Class &rect) const;

  // Computes the rectangle resulting from subtracting |rect| from |this|.  If
  // |rect| does not intersect completely in either the x- or y-direction, then
  // |*this| is returned.  If |rect| contains |this|, then an empty Rect is
  // returned.
  Class Subtract(const Class &rect) const;

  // Returns true if this rectangle equals that of the supplied rectangle.
  bool Equals(const Class &rect) const {
    return *this == rect;
  }

  // Fits as much of the receiving rectangle into the supplied rectangle as
  // possible, returning the result. For example, if the receiver had
  // a x-location of 2 and a width of 4, and the supplied rectangle had
  // an x-location of 0 with a width of 5, the returned rectangle would have
  // an x-location of 1 with a width of 4.
  Class AdjustToFit(const Class &rect) const;

  // Returns the center of this rectangle.
  PointClass CenterPoint() const;

  // Return a rectangle that has the same center point but with a size capped
  // at given |size|.
  Class Center(const SizeClass &size) const;

  // Splits |this| in two halves, |left_half| and |right_half|.
  void SplitVertically(Class *left_half, Class *right_half) const;

  // Returns true if this rectangle shares an entire edge (i.e., same width or
  // same height) with the given rectangle, and the rectangles do not overlap.
  bool SharesEdgeWith(const Class &rect) const;

 protected:
  RectBase(const PointClass &origin, const SizeClass &size);
  explicit RectBase(const SizeClass &size);
  explicit RectBase(const PointClass &origin);
  // Destructor is intentionally made non virtual and protected.
  // Do not make this public.
  ~RectBase();

 private:
  PointClass origin_;
  SizeClass size_;
};

class Rect : public RectBase<Rect, Point, Size, Insets, int> {
 public:
  Rect();
  Rect(int width, int height);
  Rect(int x, int y, int width, int height);

  explicit Rect(const base::Size &size);
  Rect(const base::Point &origin, const base::Size &size);

  ~Rect();

  std::string ToString() const;

};

} // namespace base
} // namespace media


#endif //MEDIA_BASE_RECT_H_
