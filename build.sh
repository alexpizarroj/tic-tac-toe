#!/bin/bash

set -e

server_boost_libs="-lboost_system -lboost_thread -lboost_serialization"
client_boost_libs="-lpthread "$server_boost_libs
cc_flags="-Wall -O2 -std=c++11"

echo -ne "Creating bin folder...\t"
mkdir -p bin
echo "Done."

echo -ne "Compiling Server...\t"
g++ $cc_flags $server_boost_libs -o bin/server src/ttt_server.cpp
echo "Done."

echo -ne "Compiling Client...\t"
g++ $cc_flags $client_boost_libs -o bin/client src/ttt_client.cpp
echo "Done."

echo "All is well."
