clang++ -std=c++11 -Iinclude -Weverything -Wno-c++98-compat -Wno-padded -Wno-weak-vtables -o sample sample.cpp
clang++ -std=c++11 -Iinclude -Weverything -Wno-c++98-compat -Wno-padded -Wno-weak-vtables -o sample_no_pipe2 sample_no_pipe2.cpp
clang++ -std=c++11 -Iinclude -Weverything -Wno-c++98-compat -Wno-padded -Wno-weak-vtables -o sample_post_fork_hook sample_post_fork_hook.cpp

