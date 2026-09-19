# Independent downstream consumer of the installed CreditFabric package.
#
# This project is deliberately NOT part of the Credit Fabric build: it must be
# configured on its own against an installed prefix, exactly as a third-party
# user would do.
cmake_minimum_required(VERSION 3.20)
project(CreditFabricConsumer LANGUAGES CXX)

find_package(CreditFabric 1.0 CONFIG REQUIRED)

add_executable(creditfabric_consumer main.cpp)
target_compile_features(creditfabric_consumer PRIVATE cxx_std_20)
target_link_libraries(creditfabric_consumer PRIVATE CreditFabric::creditfabric)

message(STATUS "CreditFabric consumer configured against: ${CreditFabric_DIR}")
