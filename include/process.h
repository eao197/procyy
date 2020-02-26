/**
 * @file process.h
 * @author Chase Geigle
 *
 * A simple, header-only process/pipe library for C++ on UNIX platforms.
 *
 * Released under the MIT license (see LICENSE).
 */

#ifndef PROCXX_PROCESS_H_
#define PROCXX_PROCESS_H_

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <istream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <vector>

namespace procxx
{

namespace details
{

// See https://stackoverflow.com/questions/13950938/construct-stderror-code-from-errno-on-posix-and-getlasterror-on-windows
inline std::error_code
error_code_from_errno(int errno_v)
{
    return std::make_error_code(static_cast<std::errc>(errno_v));
}

} /* namespace details */

/**
 * A special indicator for performing an operation without
 * throwing an exception.
 */
struct nothrow {};

/**
 * Represents a UNIX pipe between processes.
 */
class pipe_t
{
public:
    enum class pipe_end_enum : unsigned int
    {
        read = 0u,
        write = 1u
    };

    static constexpr pipe_end_enum READ_END = pipe_end_enum::read;
    static constexpr pipe_end_enum WRITE_END = pipe_end_enum::write;

    /**
     * Wrapper type that ensures sanity when dealing with operations on
     * the different ends of the pipe.
     */
    class pipe_end
    {
    public:
        /**
         * Constructs a new object to represent an end of a pipe.
         */
        constexpr pipe_end(pipe_end_enum end) noexcept : end_{end} {}

        /**
         * pipe_ends are implicitly convertible to ints.
         */
        constexpr operator unsigned int() const noexcept
        {
            return static_cast<unsigned int>(end_);
        }

    private:
        pipe_end_enum end_;
    };

    /**
     * Gets a pipe_end representing the read end of a pipe.
     */
    static constexpr pipe_end
    read_end() noexcept
    {
        return pipe_end{READ_END};
    }

    /**
     * Gets a pipe_end representing the write end of a pipe.
     */
    static constexpr pipe_end
    write_end() noexcept
    {
        return pipe_end{WRITE_END};
    }

    /**
     * Constructs a new pipe.
     */
    pipe_t()
    {
        // It seems that this mutex is necessary to avoid leaking
        // of file descriptors without FD_CLOEXEC set if another
        // thread calls fork()+exec().
        //
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock{mutex};

        //FIXME: the result of that function should be checked!
        ::pipe(&pipe_[0]);

        auto flags = ::fcntl(pipe_[0], F_GETFD, 0);
        ::fcntl(pipe_[0], F_SETFD, flags | FD_CLOEXEC);

        flags = ::fcntl(pipe_[1], F_GETFD, 0);
        ::fcntl(pipe_[1], F_SETFD, flags | FD_CLOEXEC);
    }

    /**
     * Pipes may be move constructed.
     */
    pipe_t(pipe_t&& other) noexcept
    {
        pipe_ = std::move(other.pipe_);
        other.pipe_[pipe_t::read_end()] = -1;
        other.pipe_[pipe_t::write_end()] = -1;
    }

    /**
     * Pipes are unique---they cannot be copied.
     */
    pipe_t(const pipe_t&) = delete;

    /**
     * Writes length bytes from buf to the pipe.
     *
     * @param buf the buffer to get bytes from
     * @param length the number of bytes to write
     */
    std::error_code
    write(
        nothrow, const char* buf, std::size_t length) noexcept
    {
        while (0u != length)
        {
            do
            {
                auto bytes = ::write(pipe_[pipe_t::write_end()], buf, length);
                if (bytes == -1)
                {
                    if (errno != EINTR)
                        return details::error_code_from_errno(errno);
                }
                else
                {
                    length -= static_cast<std::size_t>(bytes);
                    buf += bytes;
                }
            }
            // In the case of an interrupt just attempt to write again.
            while (EINTR == errno);
        }

        return {};
    }

    /**
     * Writes length bytes from buf to the pipe.
     *
     * @param buf the buffer to get bytes from
     * @param length the number of bytes to write
     */
    void
    write(const char* buf, std::size_t length)
    {
        const auto ec = write(nothrow{}, buf, length);
        if (ec)
            throw exception{"write failure: " + ec.message()};
    }

//FIXME: should this method handle EINTR?
    /**
     * Reads up to length bytes from the pipe, placing them in buf.
     *
     * @param buf the buffer to write to
     * @param length the maximum number of bytes to read
     * @return the actual number of bytes read
     */
    std::pair<std::error_code, ssize_t>
    read(
        nothrow, char* buf, uint64_t length) noexcept
    {
        auto bytes = ::read(pipe_[pipe_t::read_end()], buf, length);
        if (-1 == bytes)
            return make_pair(details::error_code_from_errno(errno), bytes);
        else
            return make_pair(std::error_code{}, bytes);
    }

    /**
     * Closes both ends of the pipe.
     */
    void
    close(nothrow nothr) noexcept
    {
        close(nothr, read_end());
        close(nothr, write_end());
    }

    /**
     * Closes a specific end of the pipe.
     */
    void
    close(nothrow, pipe_end end) noexcept
    {
        if (pipe_[end] != -1)
        {
            ::close(pipe_[end]);
            pipe_[end] = -1;
        }
    }

    /**
     * Determines if an end of the pipe is still open.
     */
    bool
    open(pipe_end end) noexcept
    {
        return pipe_[end] != -1;
    }

    /**
     * Redirects the given file descriptor to the given end of the pipe.
     *
     * @param end the end of the pipe to connect to the file descriptor
     * @param fd the file descriptor to connect
     */
    std::error_code
    dup(nothrow, pipe_end end, int fd) noexcept
    {
        if (::dup2(pipe_[end], fd) == -1)
            return details::error_code_from_errno(errno);
        else
            return {};
    }

//FIXME: should this method has a nothrow alternative?
    /**
     * Redirects the given file descriptor to the given end of the pipe.
     *
     * @param end the end of the pipe to connect to the file descriptor
     * @param fd the file descriptor to connect
     */
    void
    dup(pipe_end end, int fd)
    {
        const auto ec = dup(nothrow{}, end, fd);
        if (ec)
            throw exception{"dup failure: " + ec.message()};
    }

    /**
     * Redirects the given end of the given pipe to the current pipe.
     *
     * @param end the end of the pipe to redirect
     * @param other the pipe to redirect to the current pipe
     */
    std::error_code
    dup(nothrow nothr, pipe_end end, pipe_t& other) noexcept
    {
        return dup(nothr, end, other.pipe_[end]);
    }

    /**
     * Redirects the given end of the given pipe to the current pipe.
     *
     * @param end the end of the pipe to redirect
     * @param other the pipe to redirect to the current pipe
     */
    void
    dup(pipe_end end, pipe_t& other)
    {
        dup(end, other.pipe_[end]);
    }

    /**
     * The destructor for pipes relinquishes any file descriptors that
     * have not yet been closed.
     */
    ~pipe_t()
    {
        close(nothrow{});
    }

    /**
     * An exception type for any unrecoverable errors that occur during
     * pipe operations.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

  private:
    std::array<int, 2> pipe_;
};

/**
 * Streambuf for reading/writing to pipes.
 *
 * @see http://www.mr-edd.co.uk/blog/beginners_guide_streambuf
 */
class pipe_ostreambuf : public std::streambuf
{
  public:
    /**
     * Constructs a new streambuf, with the given buffer size and put_back
     * buffer space.
     */
    pipe_ostreambuf(size_t buffer_size = 512, size_t put_back_size = 8)
        : put_back_size_{put_back_size},
          in_buffer_(buffer_size + put_back_size)
    {
        auto end = &in_buffer_.back() + 1;
        setg(end, end, end);
    }

    ~pipe_ostreambuf() override = default;

    int_type
    underflow() override
    {
        // if the buffer is not exhausted, return the next element
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());

        auto base = &in_buffer_.front();
        auto start = base;

        // if we are not the first fill of the buffer
        if (eback() == base)
        {
            // move the put_back area to the front
            const auto dest = base;
            const auto src  = egptr() - put_back_size_ < dest ? dest : egptr() - put_back_size_;
            const auto area = static_cast<std::size_t>(egptr() - dest) < put_back_size_ ?
                static_cast<std::size_t>(egptr() - dest) : put_back_size_;
            std::memmove(dest, src, area);
            start += put_back_size_;
        }

        // start now points to the head of the usable area of the buffer
        auto read_result = stdout_pipe_.read(nothrow{},
                start,
                in_buffer_.size() - static_cast<std::size_t>(start - base));

        if (read_result.first)
        {
            //FIXME: additional information should be added to the exception.
            throw exception{"pipe read failure: "
                    + read_result.first.message()};
        }

        if (read_result.second == 0)
            return traits_type::eof();

        setg(base, start, start + read_result.second);

        return traits_type::to_int_type(*gptr());
    }

    /**
     * An exception for pipe_streambuf interactions.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

    /**
     * Gets the stdout pipe.
     */
    pipe_t&
    stdout_pipe() noexcept
    {
        return stdout_pipe_;
    }

    /**
     * Closes one of the pipes. This will flush any remaining bytes in the
     * output buffer.
     *
     * NOTE: this method doesn't throw.
     */
    virtual std::error_code
    close(nothrow, pipe_t::pipe_end end) noexcept
    {
        if (end == pipe_t::read_end())
            stdout_pipe().close(nothrow{}, pipe_t::read_end());
        return {};
    }

    /**
     * Closes one of the pipes. This will flush any remaining bytes in the
     * output buffer.
     *
     * NOTE: this method can throw.
     */
    virtual void
    close(pipe_t::pipe_end end)
    {
        auto ec = this->close(nothrow{}, end);
        if (ec)
            throw exception(ec.message());
    }

  protected:
    virtual std::error_code
    flush(nothrow) noexcept { return {}; }

    virtual void
    flush() { }

    size_t put_back_size_;
    pipe_t stdout_pipe_;
    std::vector<char> in_buffer_;
};

class pipe_streambuf : public pipe_ostreambuf
{
  public:

    pipe_streambuf(size_t buffer_size = 512, size_t put_back_size = 8)
        : pipe_ostreambuf{buffer_size, put_back_size},
          out_buffer_(buffer_size + 1)
    {
        auto begin = &out_buffer_.front();
        setp(begin, begin + out_buffer_.size() - 1);
    }

    /**
     * Destroys the streambuf, which will flush any remaining content on
     * the output buffer.
     */
    ~pipe_streambuf() override
    {
        (void)flush(nothrow{});
    }

    int_type
    overflow(int_type ch) override
    {
        if (ch != traits_type::eof())
        {
            *pptr() = static_cast<char>(ch); // safe because of -1 in setp() in ctor
            pbump(1);
            flush();
            return ch;
        }

        return traits_type::eof();
    }

    int
    sync() override
    {
        flush();
        return 0;
    }

    /**
     * Gets the stdin pipe.
     */
    pipe_t&
    stdin_pipe() noexcept
    {
        return stdin_pipe_;
    }

    std::error_code
    close(nothrow nothrow, pipe_t::pipe_end end) noexcept override
    {
        std::error_code rc_flush;
        std::error_code rc_base_close;

        if (end == pipe_t::write_end())
        {
            rc_flush = flush(nothrow);
            stdin_pipe().close(nothrow, pipe_t::write_end());
        }

        rc_base_close = pipe_ostreambuf::close(nothrow, end);

        if (rc_flush) return rc_flush;
        if (rc_base_close) return rc_base_close;

        return {};
    }

    void
    close(pipe_t::pipe_end end) override
    {
        auto rc = this->close(nothrow{}, end);
        if (rc)
            throw exception(rc.message());
    }

  private:
    std::error_code
    flush(nothrow) noexcept override
    {
        std::error_code rc{};
        if (stdin_pipe_.open(pipe_t::write_end()))
        {
            rc = stdin_pipe_.write(
                    nothrow{},
                    pbase(),
                    static_cast<std::size_t>(pptr() - pbase()));
            if (!rc)
            {
                try
                {
                    pbump(static_cast<int>(-(pptr() - pbase())));
                }
                catch(...)
                {
                    rc = std::make_error_code(std::io_errc::stream);
                }
            }
        }

        return rc;
    }

    void
    flush() override
    {
        const auto rc = flush(nothrow{});
        if (rc)
            throw exception{"flush failure: " + rc.message()};
    }

    pipe_t stdin_pipe_;
    std::vector<char> out_buffer_;
};

class process;

// Forward declaration. Will be defined later.
bool running(pid_t pid);
bool running(const process & pr);

/**
 * A handle that represents a child process.
 */
class process
{
  public:
    /**
     * Constructs a new child process, executing the given application and
     * passing the given arguments to it.
     */
    template <class... Args>
    process(std::string application, Args&&... args)
        : args_{std::move(application), std::forward<Args>(args)...},
          in_stream_{&pipe_buf_},
          out_stream_{&pipe_buf_},
          err_stream_{&err_buf_}
    {
        // nothing
    }

    /*
     * Adds an argument to the argument-list
     */
    void add_argument(std::string arg) {
        args_.push_back(std::move(arg));
    }

    /*
     * Add further arguments to the argument-list
     */
    template<typename InputIterator>
    void append_arguments(InputIterator first, InputIterator last) {
        args_.emplace(args_.end(), first, last);
    }

    /**
     * Sets the process to read from the standard output of another
     * process.
     */
    void
    read_from(process& other) noexcept
    {
        read_from_ = &other;
    }

    /**
     * Executes the process.
     *
     * This method calls child_post_fork_hook just after the return
     * from fork() call in the child process.
     */
    template<typename Hook>
    void
    exec(Hook && child_post_fork_hook)
    {
        if (pid_ != -1)
            throw exception{"process already started"};

        pipe_t err_pipe;

        auto pid = fork();
        if (pid == -1)
        {
            throw exception{"Failed to fork child process: " +
                details::error_code_from_errno(errno).message()};
        }
        else if (pid == 0)
        {
            child_post_fork_hook();

            //FIXME: should nothrow versions be used here?
            err_pipe.close(nothrow{}, pipe_t::read_end());
            pipe_buf_.stdin_pipe().close(nothrow{}, pipe_t::write_end());
            pipe_buf_.stdout_pipe().close(nothrow{}, pipe_t::read_end());
            pipe_buf_.stdout_pipe().dup(pipe_t::write_end(), STDOUT_FILENO);
            err_buf_.stdout_pipe().close(nothrow{}, pipe_t::read_end());
            err_buf_.stdout_pipe().dup(pipe_t::write_end(), STDERR_FILENO);

            if (read_from_)
            {
                read_from_->recursive_close_stdin();
                pipe_buf_.stdin_pipe().close(nothrow{}, pipe_t::read_end());
                read_from_->pipe_buf_.stdout_pipe().dup(
                        pipe_t::read_end(), STDIN_FILENO);
            }
            else
            {
                pipe_buf_.stdin_pipe().dup(pipe_t::read_end(), STDIN_FILENO);
            }

            std::vector<char*> args;
            args.reserve(args_.size() + 1);
            for (auto& arg : args_)
                args.push_back(const_cast<char*>(arg.c_str()));
            args.push_back(nullptr);

            limits_.set_limits();
            execvp(args[0], args.data());

            char err[sizeof(int)];
            std::memcpy(err, &errno, sizeof(int));
            err_pipe.write(err, sizeof(int));
            err_pipe.close(nothrow{});
            std::_Exit(EXIT_FAILURE);
        }
        else
        {
            //FIXME: should nothrow versions be used here?
            err_pipe.close(nothrow{}, pipe_t::write_end());
            pipe_buf_.stdout_pipe().close(nothrow{}, pipe_t::write_end());
            err_buf_.stdout_pipe().close(nothrow{}, pipe_t::write_end());
            pipe_buf_.stdin_pipe().close(nothrow{}, pipe_t::read_end());
            if (read_from_)
            {
                pipe_buf_.stdin_pipe().close(nothrow{}, pipe_t::write_end());
                read_from_->pipe_buf_.stdout_pipe().close(nothrow{}, pipe_t::read_end());
                read_from_->err_buf_.stdout_pipe().close(nothrow{}, pipe_t::read_end());
            }
            pid_ = pid;

            char err[sizeof(int)];
            auto read_result = err_pipe.read(nothrow{}, err, sizeof(int));
            if (!read_result.first && read_result.second == sizeof(int))
            {
                int ec = 0;
                std::memcpy(&ec, err, sizeof(int));
                throw exception{"Failed to exec process: "
                        + details::error_code_from_errno(ec).message()};
            }
            else
            {
                err_pipe.close(nothrow{});
            }
        }
    }

    /**
     * Executes the process.
     */
    void
    exec()
    {
       this->exec([]{});
    }

    /**
     * Process handles may be moved.
     */
    process(process&&) = default;

    /**
     * Process handles are unique: they may not be copied.
     */
    process(const process&) = delete;

    /**
     * The destructor for a process will wait for the child if client code
     * has not already explicitly waited for it.
     */
    ~process()
    {
        //FIXME: can wait throw?
        (void)wait(nothrow{});
    }

    /**
     * Gets the process id.
     */
    pid_t
    id() const noexcept
    {
        return pid_;
    }

    /**
     * Simple wrapper for process limit settings. Currently supports
     * setting processing time and memory usage limits.
     */
    class limits_t
    {
    public:
        /**
         * Sets the maximum amount of cpu time, in seconds.
         */
        void
        cpu_time(rlim_t max) noexcept
        {
            lim_cpu_ = true;
            cpu_.rlim_cur = cpu_.rlim_max = max;
        }

        /**
         * Sets the maximum allowed memory usage, in bytes.
         */
        void
        memory(rlim_t max) noexcept
        {
            lim_as_ = true;
            as_.rlim_cur = as_.rlim_max = max;
        }

        /**
         * Applies the set limits to the current process.
         */
        void
        set_limits()
        {
            if (lim_cpu_ && setrlimit(RLIMIT_CPU, &cpu_) != 0)
            {
                throw exception{"Failed to set cpu time limit: " +
                        details::error_code_from_errno(errno).message()};
            }

            if (lim_as_ && setrlimit(RLIMIT_AS, &as_) != 0)
            {
                throw exception{"Failed to set memory limit: " +
                        details::error_code_from_errno(errno).message()};
            }
        }

      private:
        bool lim_cpu_ = false;
        rlimit cpu_;
        bool lim_as_ = false;
        rlimit as_;
    };

    /**
     * Sets the limits for this process.
     */
    void
    limit(const limits_t& limits)
    {
        limits_ = limits;
    }

    /**
     * Waits for the child to exit.
     */
    std::error_code
    wait(nothrow) noexcept
    {
        if (!waited_)
        {
            const auto rc_pipe_buf_close =
                    pipe_buf_.close(nothrow{}, pipe_t::write_end());
            const auto rc_err_buf_close =
                    err_buf_.close(nothrow{}, pipe_t::write_end());

            // FIXME: the result of waitpid should be checked too.
            waitpid(pid_, &status_, 0);
            pid_ = -1;
            waited_ = true;

            if (rc_pipe_buf_close)
                return rc_pipe_buf_close;
            if (rc_err_buf_close)
                return rc_err_buf_close;
        }
 
        return {};
    }

    /**
     * Waits for the child to exit.
     */
    //FIXME: can it throw?
    void
    wait()
    {
        const auto rc = wait(nothrow{});
        if (rc)
            throw exception{"wait failure: " + rc.message()};
    }

    /**
     * It wait() already called?
     */
    bool
    waited() const noexcept
    {
        return waited_;
    }

    /**
     * Determines if process is running.
     */
    bool
    running() const
    {
        return ::procxx::running(*this);
    }

    /**
     * Determines if the child exited properly.
     */
    bool
    exited() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFEXITED(status_);
    }

    /**
     * Determines if the child was killed.
     */
    bool
    killed() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFSIGNALED(status_);
    }

    /**
     * Determines if the child was stopped.
     */
    bool
    stopped() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        return WIFSTOPPED(status_);
    }

    /**
     * Gets the exit code for the child. If it was killed or stopped, the
     * signal that did so is returned instead.
     */
    int
    code() const
    {
        if (!waited_)
            throw exception{"process::wait() not yet called"};
        if (exited())
            return WEXITSTATUS(status_);
        if (killed())
            return WTERMSIG(status_);
        if (stopped())
            return WSTOPSIG(status_);
        return -1;
    }

    /**
     * Closes the given end of the pipe.
     */
    std::error_code
    close(nothrow nothr, pipe_t::pipe_end end) noexcept
    {
        const auto rc_pipe_buf_close = pipe_buf_.close(nothr, end);
        const auto rc_err_buf_close = err_buf_.close(nothr, end);

        if (rc_pipe_buf_close) return rc_pipe_buf_close;
        if (rc_err_buf_close) return rc_err_buf_close;

        return {};
    }

    /**
     * Closes the given end of the pipe.
     */
    void
    close(pipe_t::pipe_end end)
    {
        const auto rc = close(nothrow{}, end);
        if (rc)
            throw exception{"close failure: " + rc.message()};
    }

    /**
     * Write operator.
     */
    template <class T>
    friend std::ostream& operator<<(process& proc, T&& input)
    {
        return proc.in_stream_ << input;
    }

    /**
     * Conversion to std::ostream.
     */
    std::ostream&
    input() noexcept
    {
        return in_stream_;
    }

    /**
     * Conversion to std::istream.
     */
    std::istream&
    output() noexcept
    {
        return out_stream_;
    }

    /**
     * Conversion to std::istream.
     */
    std::istream&
    error() noexcept
    {
        return err_stream_;
    }

    /**
     * Read operator.
     */
    template <class T>
    friend std::istream& operator>>(process& proc, T& output)
    {
        return proc.out_stream_ >> output;
    }

    /**
     * An exception type for any unrecoverable errors that occur during
     * process operations.
     */
    class exception : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };

  private:
    //FIXME: should this method has a nothrow alternative?
    void
    recursive_close_stdin()
    {
        pipe_buf_.stdin_pipe().close(nothrow{});
        if (read_from_)
            read_from_->recursive_close_stdin();
    }

    std::vector<std::string> args_;
    process* read_from_ = nullptr;
    limits_t limits_;
    pid_t pid_ = -1;
    pipe_streambuf pipe_buf_;
    pipe_ostreambuf err_buf_;
    std::ostream in_stream_;
    std::istream out_stream_;
    std::istream err_stream_;
    bool waited_ = false;
    int status_;
};

/**
 * Class that represents a pipeline of child processes. The process objects
 * that are part of the pipeline are assumed to live longer than or as long
 * as the pipeline itself---the pipeline does not take ownership of the
 * processes.
 */
class pipeline
{
  public:
    friend pipeline operator|(process& first, process& second);

    /**
     * Constructs a longer pipeline by adding an additional process.
     */
    pipeline& operator|(process& tail)
    {
        tail.read_from(processes_.back());
        processes_.emplace_back(tail);
        return *this;
    }

    /**
     * Sets limits on all processes in the pipieline.
     */
    pipeline& limit(process::limits_t limits)
    {
        for_each([limits](process& p)
                 {
                     p.limit(limits);
                 });
        return *this;
    }

    /**
     * Executes all processes in the pipeline.
     */
    void exec() const
    {
        for_each([](process& proc)
                 {
                     proc.exec();
                 });
    }

    /**
     * Obtains the process at the head of the pipeline.
     */
    process& head() const noexcept
    {
        return processes_.front();
    }

    /**
     * Obtains the process at the tail of the pipeline.
     */
    process& tail() const noexcept
    {
        return processes_.back();
    }

    /**
     * Waits for all processes in the pipeline to finish.
     */
    void wait() const
    {
        for_each([](process& proc)
                 {
                     proc.wait();
                 });
    }

    /**
     * Performs an operation on each process in the pipeline.
     */
    template <class Function>
    void for_each(Function&& function) const
    {
        for (auto& proc : processes_)
            function(proc.get());
    }

  private:
    explicit pipeline(process& head) : processes_{std::ref(head)}
    {
        // nothing
    }

    std::vector<std::reference_wrapper<process>> processes_;
};

/**
 * Begins constructing a pipeline from two processes.
 */
inline pipeline operator|(process& first, process& second)
{
    pipeline p{first};
    return p | second;
}

//FIXME: should this function has a nothrow alternative?
/**
 * Determines if process is running (zombies are seen as running).
 */
inline bool running(pid_t pid)
{
    bool result = false;
    if (pid != -1)
    {
        if (0 == ::kill(pid, 0))
        {
            int status;
            const auto r = ::waitpid(pid, &status, WNOHANG);
            if (-1 == r)
            {
                throw process::exception{"Failed to check process state "
                    "by waitpid(): "
                    + std::system_category().message(errno)};
            }
            if (r == pid)
                // Process has changed its state. We must detect why.
                result = !WIFEXITED(status) && !WIFSIGNALED(status);
            else
                // No changes in the process status. It means that
                // process is running.
                result = true;
        }
    }

    return result;
}

/**
 * Determines if process is running (zombies are seen as running).
 */
inline bool running(const process & pr)
{
    return running(pr.id());
}

}

#endif

// vim:ts=4:sts=4:sw=4:expandtab

