#ifndef LYCHEE_PIPELINE_VSYNC_ESTIMATOR_H_
#define LYCHEE_PIPELINE_VSYNC_ESTIMATOR_H_

#include <cstdint>
#include <deque>

namespace lychee {

// Recovers the display's vsync grid from the display times a caller passes,
// one per vsync. The API has no refresh rate, and callers that only know when
// they woke up pass jittered times, while frame selection needs regular
// intervals and a refresh rate within about 0.1%.
//
// The grid is a least-squares line of time over vsync number, fitted to the
// last 10 s of samples; a display time is moved onto the line, smoothed so
// that refits do not make intervals uneven.
class VsyncEstimator {
 public:
  struct Vsync {
    int64_t time;
    int64_t interval;
  };

  // The vsync |display_time| belongs to. Calls within 2 ms of the previous
  // one are the same vsync and get the same answer.
  Vsync Update(int64_t display_time);

 private:
  struct Sample {
    int64_t index;  // Vsyncs since the first sample.
    int64_t time;
  };

  void Fit();
  bool has_fit() const { return slope_ > 0; }
  // The fitted time of vsync |index|.
  int64_t TimeAt(int64_t index) const;

  std::deque<Sample> samples_;
  // time = origin + intercept_ + slope_ * (index - first index), where origin
  // is the first sample's time; doubles keep the sub-microsecond slope.
  double intercept_ = 0;
  double slope_ = 0;
  // Distance of the last few samples from the grid.
  std::deque<int64_t> recent_errors_;
  int64_t recent_error_sum_ = 0;
  // The vsync last returned, and its index.
  double grid_ = 0;
  int64_t grid_index_ = 0;
  bool grid_valid_ = false;
  int64_t last_display_time_ = 0;
  Vsync last_ = {0, 0};
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VSYNC_ESTIMATOR_H_
