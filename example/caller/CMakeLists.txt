set(SRC_LIST calleruserservice.cc ../user.pb.cc)

add_executable(consumer ${SRC_LIST})

target_link_libraries(consumer mprpc protobuf)

set(BENCH_SRC_LIST benchmark.cc ../user.pb.cc)

add_executable(benchmark ${BENCH_SRC_LIST})

target_link_libraries(benchmark mprpc protobuf pthread)