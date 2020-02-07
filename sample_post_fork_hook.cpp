#include <process.h>

#include <iostream>

int main()
{
    std::cout << "Sample PID: " << getpid() << std::endl;

    procxx::process ping( "ping", "www.google.com", "-c", "2" );
    ping.exec( [] {
        std::cout << "Child PID: " << getpid() << std::endl;
        std::cout << "Parent PID: " << getppid() << std::endl;
    } );

    std::string line;
    while( std::getline( ping.output(), line ) )
    {
        std::cout << line << std::endl;
        if( !ping.running() || !procxx::running(ping.id()) || !running(ping) )
        {
            std::cout << "not running any more" << std::endl;
            break;
        }
    }

    ping.wait();
    std::cout << "exit code: " << ping.code() << std::endl;

    return 0;
}

