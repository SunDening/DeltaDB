#include <gtest/gtest.h>

// #include "conf.h"

// TEST(ConfTest, read) {
//     auto conf = std::make_unique<delta::Config>("./conf/config.xml");
//     conf->readConf();

//     std::cout << std::format("[read conf] log_path: {}, log_base_name: {}, log_max_file_size: {}", conf->log_path,
//                              conf->log_base_name, conf->log_max_file_size)
//               << std::endl;

//     std::cout << std::format("[read conf] wal_path: {}, wal_max_file_size: {}", conf->wal_path,
//     conf->wal_max_file_size)
//               << std::endl;
// }