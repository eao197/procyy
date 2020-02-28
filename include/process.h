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

// Try to use __has_cpp_attribute if it is supported.
#if defined(__has_cpp_attribute)
	// clang-4 and clang-5 produce warnings when [[nodiscard]]
	// is used with -std=c++11 and -std=c++14.
	#if __has_cpp_attribute(nodiscard) && \
			!(defined(__clang__) && __cplusplus < 201703L)
		#define PROCXXRV_NODISCARD [[nodiscard]]
	#endif
#endif

// Handle the result of __has_cpp_attribute.
#if !defined( PROCXXRV_NODISCARD )
	#define PROCXXRV_NODISCARD
#endif

namespace procxx
{

namespace details
{

// See https://stackoverflow.com/questions/13950938/construct-stderror-code-from-errno-on-posix-and-getlasterror-on-windows
PROCXXRV_NODISCARD
inline std::error_code
error_code_from_errno(int errno_v)
{
    return std::make_error_code(static_cast<std::errc>(errno_v));
}

template<typename Lambda>
PROCXXRV_NODISCARD
std::error_code
run_as_sequence(Lambda && action)
{
    const std::error_code rc = action(); // Just for type-checking.
    return rc;
}

template<typename Lambda, typename... Tail>
PROCXXRV_NODISCARD
std::error_code
run_as_sequence(
    Lambda && action,
    Tail && ...tail)
{
    const std::error_code rc1 = action();
    const std::error_code rc2 = run_as_sequence(std::forward<Tail>(tail)...);
    return rc1 ? rc1 : rc2;
}

template<typename Exception>
void
throw_on_error(
    const char * what,
    const std::error_code ec)
{
    if (ec)
        throw Exception{what + ec.message()};
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
    PROCXXRV_NODISCARD
    static constexpr pipe_end
    read_end() noexcept
    {
        return pipe_end{READ_END};
    }

    /**
     * Gets a pipe_end representing the write end of a pipe.
     */
    PROCXXRV_NODISCARD
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

        if(-1 == ::pipe(&pipe_[0]))
            details::throw_on_error<exception>(
                    "pipe failure: ",
                    details::error_code_from_errno(errno));

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
    PROCXXRV_NODISCARD
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
        details::throw_on_error<exception>(
                "write failure: ",
                write(nothrow{}, buf, length));
    }

//FIXME: should this method handle EINTR?
    /**
     * Reads up to length bytes from the pipe, placing them in buf.
     *
     * @param buf the buffer to write to
     * @param length the maximum number of bytes to read
     * @return the actual number of bytes read
     */
    PROCXXRV_NODISCARD
    std::pair<std::error_code, ssize_t>
    read(
        nothrow, char* buf, std::size_t length) noexcept
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
    PROCXXRV_NODISCARD
    std::error_code
    close(nothrow nothr) noexcept
    {
        return details::run_as_sequence(
                [&]{ return close(nothr, read_end()); },
                [&]{ return close(nothr, write_end()); });
    }

    /**
     * Closes both ends of the pipe.
     */
    void
    close()
    {
        details::throw_on_error<exception>(
                "close failure: ",
                close(nothrow{}));
    }

    /**
     * Closes a specific end of the pipe.
     */
    PROCXXRV_NODISCARD
    std::error_code
    close(nothrow, pipe_end end) noexcept
    {
        std::error_code rc{};

        if (pipe_[end] != -1)
        {
            if (-1 == ::close(pipe_[end]))
                rc = details::error_code_from_errno(errno);

            pipe_[end] = -1;
        }

        return rc;
    }

    /**
     * Closes a specific end of the pipe.
     */
    void
    close(pipe_end end)
    {
        details::throw_on_error<exception>(
                "close(pipe_end) failure: ",
                close(nothrow{}, end));
    }

    /**
     * Determines if an end of the pipe is still open.
     */
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
    std::error_code
    dup(nothrow, pipe_end end, int fd) noexcept
    {
        if (::dup2(pipe_[end], fd) == -1)
            return details::error_code_from_errno(errno);
        else
            return {};
    }

    /**
     * Redirects the given file descriptor to the given end of the pipe.
     *
     * @param end the end of the pipe to connect to the file descriptor
     * @param fd the file descriptor to connect
     */
    void
    dup(pipe_end end, int fd)
    {
        details::throw_on_error<exception>(
                "dup failure: ",
                dup(nothrow{}, end, fd));
    }

    /**
     * Redirects the given end of the given pipe to the current pipe.
     *
     * @param end the end of the pipe to redirect
     * @param other the pipe to redirect to the current pipe
     */
    PROCXXRV_NODISCARD
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
        (void)close(nothrow{});
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

namespace details
{

//FIXME: document this!
template<typename Scalar>
struct io_helper
{
    PROCXXRV_NODISCARD
    static
    std::pair<std::error_code, Scalar>
    try_read_from(pipe_t & pipe) noexcept
    {
        std::array<char, sizeof(Scalar)> buffer;
        const auto read_result = pipe.read(
                nothrow{}, buffer.data(), buffer.size());
        if (!read_result.first)
        {
            if (read_result.second == buffer.size())
            {
                Scalar value;
                std::memcpy(&value, buffer.data(), buffer.size());
                return std::make_pair(read_result.first, value);
            }
            else
                return std::make_pair(
                        std::make_error_code(std::errc::no_message_available),
                        Scalar{});
        }
        else
            return std::make_pair(read_result.first, Scalar{});
    }

    PROCXXRV_NODISCARD
    static
    std::error_code
    try_write_to(pipe_t & pipe, const Scalar & value) noexcept
    {
        std::array<char, sizeof(Scalar)> buffer;
        std::memcpy(buffer.data(), &value, buffer.size());
        return pipe.write(nothrow{}, buffer.data(), buffer.size());
    }
};

template<std::size_t N>
struct io_helper<std::array<char, N>>
{
    using array_type = std::array<char, N>;

    PROCXXRV_NODISCARD
    static
    std::pair<std::error_code, array_type>
    try_read_from(pipe_t & pipe) noexcept
    {
        std::pair<std::error_code, array_type> result;
        const auto read_result = pipe.read(nothrow{},
                result.second.data(), result.second.size());
        if (!read_result.first && read_result.second != result.second.size())
        {
            result.first = std::make_error_code(std::errc::no_message_available);
        }
        else
            result.first = read_result.first;

        return result;
    }

    PROCXXRV_NODISCARD
    static
    std::error_code
    try_write_to(pipe_t & pipe, const array_type & value) noexcept
    {
        return pipe.write(nothrow{}, value.data(), value.size());
    }
};

} /* namespace details */

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
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
    virtual std::error_code
    close(nothrow, pipe_t::pipe_end end) noexcept
    {
        std::error_code rc{};
        if (end == pipe_t::read_end())
            rc = stdout_pipe().close(nothrow{}, pipe_t::read_end());
        return rc;
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
        details::throw_on_error<exception>(
                "close(pipe_end) failure: ",
                this->close(nothrow{}, end));
    }

  protected:
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
    pipe_t&
    stdin_pipe() noexcept
    {
        return stdin_pipe_;
    }

    PROCXXRV_NODISCARD
    std::error_code
    close(nothrow nothrow, pipe_t::pipe_end end) noexcept override
    {
        return details::run_as_sequence(
                [&]{
                    if (end == pipe_t::write_end())
                        return details::run_as_sequence(
                            [&]{ return flush(nothrow); },
                            [&]{ return stdin_pipe().close(
                                    nothrow, pipe_t::write_end()); });
                    else
                        return std::error_code{};
                },
                [&]{ return pipe_ostreambuf::close(nothrow, end); });
    }

    void
    close(pipe_t::pipe_end end) override
    {
        details::throw_on_error<exception>(
                "close(pipe_end) failure: ",
                this->close(nothrow{}, end));
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
        details::throw_on_error<exception>(
                "flush failure: ",
                flush(nothrow{}));
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
    //FIXME: document this!
    enum class failure_reason : int
    {
        standard_exception = 0,
        unknown_exception = 1,
        execvp_failure = 2
    };

    //FIXME: document this!
    static constexpr std::size_t error_message_buffer_size = 128;

public:
    //FIXME: document this!
    enum class hook_place { child, parent };

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

//FIXME: document format of Hook.
    /**
     * Executes the process.
     *
     * This method calls post_fork_hook just after the return from fork().
     */
    template<typename Hook>
    void
    exec(Hook && post_fork_hook)
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
            on_exec_in_child(std::forward<Hook>(post_fork_hook), err_pipe);
        }
        else
        {
            // NOTE: the pid of the child should be stored.
            // It marks the `process` object as `started`. And that allows
            // to wait for the child process, check its status, kill it
            // and so on.
            pid_ = pid;

            on_exec_in_parent(std::forward<Hook>(post_fork_hook), err_pipe);
        }
    }

    /**
     * Executes the process.
     */
    void
    exec()
    {
       this->exec([](hook_place){});
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
        (void)wait(nothrow{});
    }

    /**
     * Gets the process id.
     */
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
    std::error_code
    wait(nothrow) noexcept
    {
        std::error_code rc{};
        if (!waited_)
        {
            rc = details::run_as_sequence(
                    [&]{ return pipe_buf_.close(
                            nothrow{}, pipe_t::write_end()); },
                    [&]{ return err_buf_.close(
                            nothrow{}, pipe_t::write_end()); },
                    [&]{
                        const auto r = waitpid(pid_, &status_, 0);
                        return -1 == r ?
                                details::error_code_from_errno(errno) :
                                std::error_code{};
                    } );

            pid_ = -1;
            waited_ = true;
        }
 
        return rc;
    }

    /**
     * Waits for the child to exit.
     */
    void
    wait()
    {
        details::throw_on_error<exception>(
                "wait failure: ",
                wait(nothrow{}));
    }

    /**
     * It wait() already called?
     */
    PROCXXRV_NODISCARD
    bool
    waited() const noexcept
    {
        return waited_;
    }

    /**
     * Determines if process is running.
     */
    PROCXXRV_NODISCARD
    bool
    running() const
    {
        return ::procxx::running(*this);
    }

    /**
     * Determines if the child exited properly.
     */
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
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
    PROCXXRV_NODISCARD
    std::error_code
    close(nothrow nothr, pipe_t::pipe_end end) noexcept
    {
        return details::run_as_sequence(
                [&]{ return pipe_buf_.close(nothr, end); },
                [&]{ return err_buf_.close(nothr, end); });
    }

    /**
     * Closes the given end of the pipe.
     */
    void
    close(pipe_t::pipe_end end)
    {
        details::throw_on_error<exception>(
                "close(pipe_end) failure: ",
                close(nothrow{}, end));
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
    PROCXXRV_NODISCARD
    std::ostream&
    input() noexcept
    {
        return in_stream_;
    }

    /**
     * Conversion to std::istream.
     */
    PROCXXRV_NODISCARD
    std::istream&
    output() noexcept
    {
        return out_stream_;
    }

    /**
     * Conversion to std::istream.
     */
    PROCXXRV_NODISCARD
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
    void
    recursive_close_stdin(nothrow nothr) noexcept
    {
        (void)pipe_buf_.stdin_pipe().close(nothr);
        if (read_from_)
            read_from_->recursive_close_stdin(nothr);
    }

    template<typename Hook>
    void
    on_exec_in_child(
        Hook && post_fork_hook,
        pipe_t & err_pipe)
    {
        failure_reason reason = failure_reason::execvp_failure;
        std::array<char, error_message_buffer_size> error_message;

        try
        {
            post_fork_hook(hook_place::child);

            close_pipes_in_child(err_pipe);

            limits_.set_limits();

            auto args = make_args_vector_for_execvp();
            execvp(args[0], args.data());
        }
        catch(const std::exception & x)
        {
            reason = failure_reason::standard_exception;
            std::strncpy(error_message.data(), x.what(),
                    error_message_buffer_size - 1u);
        }
        catch(...)
        {
            reason = failure_reason::unknown_exception;
        }

        transfer_error_info_to_parent(err_pipe, reason, error_message);

        std::_Exit(EXIT_FAILURE);
    }

    void
    close_pipes_in_child(
        pipe_t & err_pipe)
    {
        err_pipe.close(pipe_t::read_end());
        pipe_buf_.stdin_pipe().close(pipe_t::write_end());
        pipe_buf_.stdout_pipe().close(pipe_t::read_end());
        pipe_buf_.stdout_pipe().dup(pipe_t::write_end(), STDOUT_FILENO);
        err_buf_.stdout_pipe().close(pipe_t::read_end());
        err_buf_.stdout_pipe().dup(pipe_t::write_end(), STDERR_FILENO);

        if (read_from_)
        {
            // NOTE: recursive_close_stdin has no throwing version.
            read_from_->recursive_close_stdin(nothrow{});

            pipe_buf_.stdin_pipe().close(pipe_t::read_end());
            read_from_->pipe_buf_.stdout_pipe().dup(
                    pipe_t::read_end(), STDIN_FILENO);
        }
        else
        {
            pipe_buf_.stdin_pipe().dup(pipe_t::read_end(), STDIN_FILENO);
        }
    }

    std::vector<char*>
    make_args_vector_for_execvp() const
    {
        std::vector<char*> args;
        args.reserve(args_.size() + 1);
        for (auto& arg : args_)
            args.push_back(const_cast<char*>(arg.c_str()));
        args.push_back(nullptr);

        return args;
    }

    static void
    transfer_error_info_to_parent(
        pipe_t & err_pipe,
        failure_reason reason,
        std::array<char, error_message_buffer_size> error_message) noexcept
    {
        //FIXME: this format should be documented!
        (void)details::io_helper<failure_reason>::try_write_to(
                err_pipe, reason);

        switch(reason)
        {
            case failure_reason::standard_exception: {
                (void)details::io_helper<
                        std::array<char, error_message_buffer_size>
                    >::try_write_to(err_pipe, error_message);
            }
            break;

            case failure_reason::unknown_exception: break; // Nothing to do.

            case failure_reason::execvp_failure: {
                (void)details::io_helper<int>::try_write_to(err_pipe, errno);
            }
        }

        (void)err_pipe.close(nothrow{});
    }

    template<typename Hook>
    void
    on_exec_in_parent(
        Hook && post_fork_hook,
        pipe_t & err_pipe)
    {
        //FIXME: should exceptions to be handled?
        post_fork_hook(hook_place::parent);

        close_pipes_in_parent(err_pipe);

        const auto try_reason = details::io_helper<failure_reason>::
                try_read_from(err_pipe);
        if (!try_reason.first)
        {
            if (failure_reason::standard_exception == try_reason.second)
            {
                const auto try_error_message = details::io_helper<
                        std::array<char, error_message_buffer_size>
                    >::try_read_from(err_pipe);
                if (!try_error_message.first)
                {
                    const auto & msg = try_error_message.second;
                    throw exception("exception in child: " + std::string{
                            msg.data(), msg.size()});
                }
                else
                    throw exception("exception in child (description is "
                            "not available)");
            }
            else if (failure_reason::unknown_exception == try_reason.second)
            {
                throw exception("unknown exception in child (description "
                        "is not available)");
            }
            else if (failure_reason::execvp_failure == try_reason.second)
            {
                const auto try_errno = details::io_helper<int>::
                    try_read_from(err_pipe);
                if (!try_errno.first)
                {
                    details::throw_on_error<exception>(
                            "execvp failure: ",
                            details::error_code_from_errno(try_errno.second));
                }
                else
                    throw exception("execvp failure (description is "
                            "not available)");
            }
            else
                throw exception("unknown failure in child");
        }
    }

    void
    close_pipes_in_parent(pipe_t & err_pipe) noexcept
    {
        //
        // NOTE: non-throwing versions are used here.
        // It's because the `exec()` in parent process should throw
        // only if the child can't launch the process specified.
        // All other errors just ignored to allow calls to `wait`,
        // `id`, `running` and other methods of `process` class that
        // are actual for controlling the running child.
        //
        (void)err_pipe.close(nothrow{}, pipe_t::write_end());
        (void)pipe_buf_.stdout_pipe().close(nothrow{}, pipe_t::write_end());
        (void)err_buf_.stdout_pipe().close(nothrow{}, pipe_t::write_end());
        (void)pipe_buf_.stdin_pipe().close(nothrow{}, pipe_t::read_end());
        if (read_from_)
        {
            (void)pipe_buf_.stdin_pipe().close(
                    nothrow{}, pipe_t::write_end());
            (void)read_from_->pipe_buf_.stdout_pipe().close(
                    nothrow{}, pipe_t::read_end());
            (void)read_from_->err_buf_.stdout_pipe().close(
                    nothrow{}, pipe_t::read_end());
        }
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
inline bool
running(pid_t pid)
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
inline bool
running(const process & pr)
{
    return running(pr.id());
}

}

#endif

// vim:ts=4:sts=4:sw=4:expandtab

