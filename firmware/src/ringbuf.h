#pragma once
#include <stddef.h>
#include <stdint.h>

// Single-producer / single-consumer byte ring buffer (usable capacity N-1).
template <size_t N>
class RingBuf {
 public:
  size_t used() const { return (head_ + N - tail_) % N; }
  size_t space() const { return N - 1 - used(); }
  bool empty() const { return head_ == tail_; }
  void clear() { tail_ = head_; }

  size_t write(const uint8_t* d, size_t n) {
    size_t w = 0;
    size_t h = head_;
    while (w < n && ((h + 1) % N) != tail_) {
      buf_[h] = d[w++];
      h = (h + 1) % N;
    }
    head_ = h;
    return w;
  }
  bool put(uint8_t c) { return write(&c, 1) == 1; }

  size_t peek(uint8_t* d, size_t n) const {
    size_t r = 0, t = tail_;
    while (r < n && t != head_) {
      d[r++] = buf_[t];
      t = (t + 1) % N;
    }
    return r;
  }
  void drop(size_t n) {
    size_t u = used();
    if (n > u) n = u;
    tail_ = (tail_ + n) % N;
  }
  // Consumer side: put bytes back in front of the unread data (they are read again first).
  size_t unread(const uint8_t* d, size_t n) {
    size_t k = 0, t = tail_;
    while (k < n && ((t + N - 1) % N) != head_) {
      t = (t + N - 1) % N;
      buf_[t] = d[n - 1 - k];
      k++;
    }
    tail_ = t;
    return k;
  }
  size_t read(uint8_t* d, size_t n) {
    size_t r = peek(d, n);
    drop(r);
    return r;
  }

 private:
  uint8_t buf_[N];
  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;
};
