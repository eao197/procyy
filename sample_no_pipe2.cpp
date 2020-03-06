#define PROCYY_HAS_PIPE2 0
#include <process.h>

#include <iostream>

int main()
{
    procyy::process ping( "ping", "www.google.com", "-c", "2" );
    ping.exec();

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

    return 0;
}

