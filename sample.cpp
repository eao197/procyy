#include <process.h>

#include <iostream>

// An instance of `process` can be passed by value
// (due to move constructor/operator of `process` class).
static void handle_running_ping(procyy::process ping)
{
    std::string line;
    while( std::getline( ping.output(), line ) )
    {
        std::cout << line << std::endl;
        if( !ping.running() )
        {
            std::cout << "not running any more" << std::endl;
            break;
        }
    }

    ping.wait();
    std::cout << "exit code: " << ping.code() << std::endl;
}

int main()
{
    procyy::process ping( "ping", "www.google.com", "-c", "2" );
    ping.exec();

    handle_running_ping( std::move(ping) );

    return 0;
}

