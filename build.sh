#!/bin/bash

boost_server_libs="-lboost_system -lboost_thread -lboost_serialization"
boost_client_libs=$boost_server_libs" -lpthread"

echo -n "Creating bin folder... " &&
rm -f -r bin &&
mkdir bin &&
echo "Done." &&

echo -n "Compiling Server...    " &&
g++ -Wall -O2 -std=c++11 $boost_server_libs -o bin/server src/ttt_server.cpp &&
echo "Done." &&

echo -n "Compiling Client...    " &&
g++ -Wall -O2 -std=c++11 $boost_client_libs -o bin/client src/ttt_client.cpp &&
echo "Done." &&

echo "All is well."