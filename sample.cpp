#include <process.h>

#include <iostream>
#include <list>

// An instance of `process` can be passed by value
// (due to move constructor/operator of `process` class).
static void handle_running_cmd(procyy::process cmd)
{
    std::string line;
    while( std::getline( cmd.output(), line ) )
    {
        std::cout << line << std::endl;
    }

    if( !cmd.running() )
    {
        std::cout << "-----\n"
            "not running any more" << std::endl;
    }

    cmd.wait();
    std::cout << "exit code: " << cmd.code() << std::endl;
}

int main()
{
    procyy::process cmd( "ls", "--almost-all", "-F" );

    std::list<std::string> additional_args{
        "--color=auto",
        "--human-readable",
        "--time-style=long-iso"
    };
    cmd.add_some_arguments(additional_args.begin(), additional_args.end());

    std::string various_args{ "-l,-t" };
    cmd.emplace_argument(various_args.begin(), various_args.begin()+2);
    cmd.emplace_argument(various_args.begin()+3, various_args.end());

    cmd.exec();

    handle_running_cmd( std::move(cmd) );

    return 0;
}

