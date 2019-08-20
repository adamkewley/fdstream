#pragma once

#include <istream>

/**
 * fdistream: custom istream implementation that adds support for
 * high-perf forward-seeking.
 */
namespace ak {
  class fdbuf;

  class fdistream : public std::istream {
  public:
    fdistream(const std::string& pth);
    fdistream(int fd);
    fdistream(const fdistream& other) = delete;
    fdistream(fdistream&& tmp) = delete;
    ~fdistream() noexcept;

    fdistream& operator=(const fdistream& other) = delete;
    fdistream& operator=(fdistream&& tmp) = delete;
  private:
    fdbuf* buf;
  };
}
