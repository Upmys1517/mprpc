aux_source_directory(. SRC_LIST)

#set(SRC_LIST mprpcapplication.cc mprpcconfig.cc rpcheader.pb.cc rpcprovider.cc)

#允许指定非系统安装的 protobuf：cmake -DPROTOBUF_ROOT=/path/to/protobuf
if(PROTOBUF_ROOT)
    include_directories(${PROTOBUF_ROOT}/include)
    link_directories(${PROTOBUF_ROOT}/lib)
endif()

#编译生成静态库
add_library(mprpc ${SRC_LIST})

target_link_libraries(mprpc muduo_net muduo_base protobuf pthread zookeeper_mt)
