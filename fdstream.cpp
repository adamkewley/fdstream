#include "fdstream.hpp"

#include <sstream>
#include <stdexcept>
#include <cstring>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace {

  constexpr size_t default_bufsize = 1<<13;

  enum class FdType {
    reg,
    fifo,
    other,
  };

  class Fd {
  public:
    Fd(int _fd): fd{_fd}, owned{false} {
      struct stat s;
      if (::fstat(this->fd, &s) != -1) {
        if (s.st_mode & S_IFREG) {
          this->type = FdType::reg;
        } else if (s.st_mode & S_IFIFO) {
          this->type = FdType::fifo;
        } else {
          this->type = FdType::other;
        }
      } else {
        std::stringstream msg;
        msg << "fd " << fd << ": " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }

    Fd(const std::string& pth, int flags): owned{true} {
      while ((this->fd = ::open(pth.c_str(), flags)) == -1) {
        if (errno != EINTR) {
          std::stringstream msg;
          msg << pth << ": " << strerror(errno);
          throw std::runtime_error{msg.str()};
        }
      }

      struct stat s;
      if (::fstat(this->fd, &s) != -1) {
        if (s.st_mode & S_IFREG) {
          this->type = FdType::reg;
        } else if (s.st_mode & S_IFIFO) {
          this->type = FdType::fifo;
        } else {
          this->type = FdType::other;
        }
      } else {
        ::close(this->fd);
        std::stringstream msg;
        msg << pth << ": " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }

    Fd(const std::string& pth): Fd{pth, O_RDONLY} {
    }

    Fd(const Fd& other) = delete;
    Fd(Fd&& tmp) = delete;

    ~Fd() noexcept {
      if (this->owned) {
        ::close(fd);
      }
    }

    Fd& operator=(const Fd& other) = delete;
    Fd& operator=(Fd&& tmp) = delete;

    friend size_t read(Fd& fd, void* buf, size_t count);
    friend void lseek(Fd& fd, off_t offset, int whence);
    friend void skip(Fd& fd, size_t n);
    friend void splice_pipe_to_null(Fd& fd, size_t n);
    friend void splice_to_null(Fd& fd, size_t n);

  private:
    int fd;
    FdType type;
    bool owned;
  };

  class Pipe {
  public:
    Pipe() {
      int pipefd[2];
      int ret = ::pipe(pipefd);

      if (ret != -1) {
        this->read = pipefd[0];
        this->write = pipefd[1];
      } else {
        std::stringstream msg;
        msg << "error calling pipe: " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }

    Pipe(const Pipe& other) = delete;
    Pipe(Pipe&& tmp) = delete;

    ~Pipe() noexcept {
      ::close(read);
      ::close(write);
    }

    Pipe& operator=(const Pipe& other) = delete;
    Pipe& operator=(Pipe&& tmp) = delete;

    friend void splice_to_null(Fd& fd, size_t n);

  private:
    int read;
    int write;
  };

  size_t read(Fd& fd, void* buf, size_t count) {
    ssize_t n;
    while ((n = ::read(fd.fd, buf, count)) == -1) {
      if (errno != EINTR) {
        std::stringstream msg;
        msg << "read error: " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }
    return n;
  }

  void skip(Fd& fd, size_t n) {
    switch (fd.type) {
    case FdType::reg:
      lseek(fd, n,  SEEK_CUR);
      break;
    case FdType::fifo:
      splice_pipe_to_null(fd, n);
      break;
    default:
      splice_to_null(fd, n);
      break;
    }
  }

  void lseek(Fd& fd, off_t offset, int whence) {
    off_t o = ::lseek(fd.fd, offset, whence);

    if (o != -1) {
      return;
    } else {
      std::stringstream msg;
      msg << "seek error: " << strerror(errno);
      throw std::runtime_error{msg.str()};
    }
  }

  // input assertion: fd is a pipe and, therefore, can be `splice(2)`d
  // directly into `/dev/null` in the kernel
  void splice_pipe_to_null(Fd& fd, size_t n) {
    thread_local Fd dev_null{"/dev/null", O_WRONLY};

    while (n > 0) {
      ssize_t read = ::splice(fd.fd, NULL, dev_null.fd, NULL, n, 0);
      if (read != -1) {
        n -= read;
        continue;
      } else {
        std::stringstream msg;
        msg << "splice_pipe_to_null failed: " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }
  }

  // This approach works with all fd types because it creates a
  // (in-kernel) pipe and shuffles data (zero copy) kernel-side. In
  // effect, it's the same as a traditional `read(2)` + `write(2)`
  // with the limitation (and benefit) that the data is never copied
  // to userspace.
  void splice_to_null(Fd& fd, size_t n) {
    thread_local Fd dev_null{"/dev/null", O_WRONLY};
    thread_local Pipe p;

    while (n > 0) {
      ssize_t read = ::splice(fd.fd, NULL, p.write, NULL, n, 0);

      if (read > 0) {
        ssize_t written = ::splice(p.read, NULL, dev_null.fd, NULL, read, 0);
        if (written != -1) {
          n -= read;
          break;
        } else {
          std::stringstream msg;
          msg << "splice write failed: " << strerror(errno);
          throw std::runtime_error{msg.str()};
        }
      } else if (read == 0) {
        std::stringstream msg;
        msg << "splice read prematurely returned 0 bytes read (expected "
            << n << " bytes to be read)";
        throw std::runtime_error{msg.str()};
      } else {
        std::stringstream msg;
        msg << "splice1 failed: " << strerror(errno);
        throw std::runtime_error{msg.str()};
      }
    }
  }
}

namespace ak {
  class fdbuf : public std::streambuf {
  public:
    fdbuf(const std::string& pth): fd{pth} {
      this->setg(buf, buf, buf);
    }

    fdbuf(int _fd): fd{_fd} {
      this->setg(buf, buf, buf);
    }

    fdbuf(const fdbuf& other) = delete;
    fdbuf(fdbuf&& tmp) = delete;

    ~fdbuf() noexcept {
      if (this->owned) {
        delete[] this->buf;
      }
    }

    fdbuf& operator=(const fdbuf& other) = delete;
    fdbuf& operator=(fdbuf&& tmp) = delete;

    std::streamsize xsgetn(char* s, std::streamsize n) override {
      std::streamsize rem = n;

      while (rem > 0) {
        if (this->gptr() == this->egptr()) {
          if (this->underflow() == std::char_traits<char>::eof()) {
            return n - rem;
          }
        }

        auto avail = this->egptr() - this->gptr();
        auto amt = std::min(rem, avail);

        std::copy(this->gptr(), this->gptr() + amt, s);
        this->setg(this->eback(), this->gptr() + amt, this->egptr());
        s += amt;
        rem -= amt;
      }

      return n - rem;
    }
    std::streampos seekoff(std::streamoff off,
                           std::ios::seekdir way,
                           std::ios::openmode which = std::ios::in | std::ios::out) override {
      // This optimization is specifically for seeking forward
      if (which == std::ios::out || way != std::ios::cur) {
        return std::streambuf::seekoff(off, way, which);
      }

      auto end = this->egptr();
      auto buffered = end - this->gptr();
      this->setg(this->eback(), end, end);
      skip(this->fd, off - buffered);

      return this->gptr() - this->eback();
    }

    int underflow() override {
      if (this->gptr() == this->egptr()) {
        size_t n = read(fd, buf, capacity);
        this->setg(buf, buf, buf + n);
      }

      return this->gptr() == this->egptr()
        ? std::char_traits<char>::eof()
        : std::char_traits<char>::to_int_type(*this->gptr());
    }

    std::streambuf* setbuf(char* s, std::streamsize n) override {
      if (this->owned) {
        delete[] buf;
      }

      this->owned = false;
      this->buf = s;
      this->capacity = n;

      return this;
    }

  private:
    Fd fd;
    char* buf = new char[default_bufsize];
    size_t capacity = default_bufsize;
    bool owned = true;
  };

  fdistream::fdistream(const std::string& pth): buf{new fdbuf(pth)} {
    this->rdbuf(buf);
  }

  fdistream::fdistream(int fd): buf{new fdbuf(fd)} {
    this->rdbuf(buf);
  }

  fdistream::~fdistream() noexcept {
    delete buf;
  }
}
