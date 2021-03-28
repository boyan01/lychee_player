//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_BASE_SIZE_H_
#define MEDIA_BASE_SIZE_H_

#include <string>

namespace media {
namespace base {

// A size has width and height values.
template<typename Class, typename Type>
class SizeBase {
 public:
  Type width() const { return width_; }
  Type height() const { return height_; }

  Type GetArea() const { return width_ * height_; }

  void SetSize(Type width, Type height) {
    set_width(width);
    set_height(height);
  }

  void Enlarge(Type width, Type height) {
    set_width(width_ + width);
    set_height(height_ + height);
  }

  Class Scale(float scale) const {
    return Scale(scale, scale);
  }

  Class Scale(float x_scale, float y_scale) const {
    return Class(static_cast<Type>(width_ * x_scale),
                 static_cast<Type>(height_ * y_scale));
  }

  void set_width(Type width);
  void set_height(Type height);

  bool operator==(const Class &s) const {
    return width_ == s.width_ && height_ == s.height_;
  }

  bool operator!=(const Class &s) const {
    return *this != s;
  }

  bool IsEmpty() const {
    // Size doesn't allow negative dimensions, so testing for 0 is enough.
    return (width_ == 0) || (height_ == 0);
  }

 protected:
  SizeBase(Type width, Type height);
  // Destructor is intentionally made non virtual and protected.
  // Do not make this public.
  virtual ~SizeBase();

 private:
  Type width_;
  Type height_;
};

class Size : public SizeBase<Size, int> {
 public:
  Size();

  Size(int width, int height);

  ~Size() override;

  std::string ToString() const;

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_SIZE_H_
