#include "test.pb.h"
#include <iostream>
#include <string>


int main1(){
    //封装了login请求对象的数据
    fixbug::LoginRequest req;
    req.set_username("zhangsan");
    req.set_password("123456");

    //对象数据序列化成字符串(std::string)
    std::string send_str;
    if(req.SerializeToString(&send_str)){
        std::cout << send_str << std::endl;
    }

    //从send_str反序列化一个login请求对象
    fixbug::LoginRequest req2;
    if(req2.ParseFromString(send_str)){
        std::cout << "username: " << req2.username() << std::endl;
        std::cout << "password: " << req2.password() << std::endl;
    }

    return 0;
}


int main(){
    // fixbug::LoginResponse req;
    // fixbug::Result *rc = req.mutable_result();
    // rc->set_errcode(1);
    // rc->set_errmsg("登录处理失败了");

    fixbug::GetFriendListsResponse rsp;
    fixbug::Result *rc = rsp.mutable_result();
    rc->set_errcode(0);
    rc->set_errmsg("获取好友列表成功了");

    fixbug::User *user1 = rsp.add_friend_list();
    user1->set_name("zhangsan");
    user1->set_age(20);
    user1->set_sex(fixbug::User::male);

    fixbug::User *user2 = rsp.add_friend_list();
    user2->set_name("cuihua");
    user2->set_age(22);
    user2->set_sex(fixbug::User::female);

    std::cout << rsp.friend_list_size() << std::endl;


    return 0;
}