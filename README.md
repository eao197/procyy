# procyy

A simple process management library for C++ on UNIX platforms.

This is a reincarnation of small, simple and easy to use library
[procxx](https://github.com/skystrife/procxx) that seems to be abandoned.

## Usage
Here is a simple (toy) example of setting up a very basic pipeline.

```cpp
// construct a child process that runs `cat`
procyy::process cat{"cat"};

// construct a child process that runs `wc -c`
procyy::process wc{"wc", "-c"};

// set up the pipeline and execute the child processes
(cat | wc).exec();

// write "hello world" to the standard input of the cat child process
cat << "hello world";

// close the write end (stdin) of the cat child
cat.close(procyy::pipe_t::write_end());

// read from the `wc -c` process's stdout, line by line
std::string line;
while (std::getline(wc.output(), line))
    std::cout << line << std::endl;
```

procyy also provides functionality for setting resource limits on the
child processes, as demonstrated below. The functionality is implemented
via the POSIX `rlimit` functions.

```cpp
procyy::process cat{"cat"};
procyy::process wc{"wc", "-c"};

// OPTION 1: same limits for all processes in the pipeline
procyy::process::limits_t limits;
limits.cpu_time(3);         // 3 second execution time limit
limits.memory(1024*1024*1); // 1 MB memory usage limit
(cat | wc).limit(limits).exec();

// OPTION 2: individual limits for each process
procyy::process::limits_t limits;
limits.cpu_time(3);         // 3 second execution time limit
limits.memory(1024*1024*1); // 1 MB memory usage limit
wc.limit(limits);

procyy::process::limits_t limits;
limits.cpu_time(1);  // 1 second execution time limit
limits.memory(1024); // 1 KB memory usage limit
cat.limit(limits);

(cat | wc).exec();
```

# Differences from the original procxx

## procyy namespace instead of procxx namespace

The whole content of library is now in `procyy` namespace instead of `procxx`.
So it's necessary to do search and replace during the switch to procyy.

## There are throwing and non-throwing methods

Some methods of procyy's classes now have two forms. The first one throws
exceptions and looks like in procxx. For example:

```cpp
namespace procyy {

class pipe_t {
public:
    ...
    void write(const char* buf, std::size_t length);
    void close();
    void close(pipe_end end);
    void dup(pipe_end end, int fd);
    ...
};

...
}
```
Those methods work just like in procxx.

The second form doesn't throw exceptions and returns either `std::error_code`
or `std::pair<std::error_code, some-type>`. For example:

```cpp
namespace procyy {

class pipe_t {
public:
    ...
    std::error_code
    write(nothrow, const char* buf, std::size_t length) noexcept;

    std::pair<std::error_code, ssize_t>
    read(nothrow, char* buf, std::size_t length) noexcept;

    std::error_code
    close(nothrow nothr) noexcept;

    std::error_code
    close(nothrow, pipe_end end) noexcept;

    std::error_code
    dup(nothrow, pipe_end end, int fd) noexcept;
    ...
};

...
}
```
Those methods receive an object of type `procyy::nothrow` as the first
argument.

## There is no throwing pipe_t::read method

This method has been removed. Only non-throwing `pipe_t::read` is available
now.

If you need throwing version of `pipe_t::read` method please open an issue.

## The constructor of pipe_t::pipe_end doesn't throw now

The constructor of `pipe_t::pipe_end` in procxx accepts `unsigned int` and
throws an exception for an invalid value.

In procyy constants `pipe_t::READ_END` and `pipe_t::WRITE_END` have type
`pipe_t::pipe_end_enum` and is not `unsigned int` anymore. The constructor of
`pipe_t::pipe_end` now accepts `pipe_end_enum` instead of `unsigned int` and
doesn't check the passed value. So there is no need to throw an exception.

## Non-throwing methods are used during process instance cleanup

The procxx library had an issue: throwing versions of `flush` and `close`
methods were called during the cleanup of `process` instances. This leads to the
termination of the application if the destructor of `process` throws during the
stack unwinding due to another exception.

The procyy use non-throwing methods in `procyy::process` cleanup routines.

## Another overload of process::exec

There is another overload of `process::exec` that accepts a callback. This
callback will be called just after a successful return from `fork`. Callback
will be called in the parent and the child processes.

```cpp
procyy::process ping( "ping", "www.google.com", "-c", "2" );
ping.exec( [](procyy::process::hook_place where) {
    if(procyy::process::hook_place::child == where)
    {
        // This is callback in the child process.
        std::cout << "Child PID: " << getpid() << std::endl;
        std::cout << "Parent PID: " << getppid() << std::endl;
    }
} );
```

This callback can throw exceptions in the child process. In that case the work
of the child process will be finished (an `execvp` in the child process won't
be called).

If a callback function throws in the parent process then the exception thrown
will just be ignored.

## There is no free function running(process)

There is no more free function `running()` that accepts a reference to
`process` class. The `process::running()` method should be used instead.  It's
becase `running` can change the state of `process` instance (`waitpid` is
called inside `running` and this call changes `waited` flag inside `process`
instance).

## An extended way of posting errors from a child process to the parent

The procxx library uses additional pipe to report of `execvp` call error to the
parent process. Only one integer value is sent to that pipe in the case of an
error.

The procyy library uses the same scheme, but additional data can be posted from
a child to the parent process. For example, if an exception is thrown during
the preparation to `execvp` call then some description of that error will be
written to that pipe (if the exception class is derived from `std::exception`
this additional data will be the first 127 bytes of string returned by
`std::exception::what()`.

## Usage of pipe2 in this version of procyy

The procxx library has `PROCXX_HAS_PIPE2` feature macro and use `pipe2` call in
the constructor of `pipe_t` class if `PROCXX_HAS_PIPE2` is defined to non-zero
value. And `PROCXX_HAS_PIPE2` is defined to 1 by default without an attempt to
detect the current platform and the presence of `pipe2` call.

The procyy library has the similar `PROCYY_HAS_PIPE2` macro, but if it is not
definied explicitly then procyy tries to detect the platform. If it is Linux or
FreeBSD 10 or above then `PROCYY_HAS_PIPE2` will be automatically defined to 1
and `pipe2` call will be used in the constructor of `procyy::pipe_t` class.

If `PROCYY_HAS_PIPE2` is explicitly defined to 0, or if procyy is compiled on
some different platform (like macOS) then `pipe`+`fcntl` will be used in the
constructor of `procyy::pipe_t` class.

If such behaviour is an issue for you please provide a PR.

### std::mutex is not used around a call to pipe()

The procxx library uses a fallback to a sequence of `pipe` and `fcntl` calls if
`pipe2` is not available. This sequence is guarded by a static mutex instance.
This guard is intended to add some safety for multithreaded code (see some
explanations
[here](https://github.com/skystrife/procxx/commit/afe17ba37341528bbbee9d96c5b68b1fe154fad3)).

But the problem is that I don't understand how this mutex can prevent the leak
of pipe descriptors if another thread performs `fork`+`execvp`.

So I think this mutex is just useless. And because of that the constructor if
`procyy::pipe_t` doesn't use a lock of a mutex around calls to `pipe`+`fcntl`.

