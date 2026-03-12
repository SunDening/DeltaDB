#include <assert.h>
#include <stdio.h>
#include <tinyxml.h>
#include <format>

#include "conf.h"

namespace delta {

// 构造函数 - 加载XML文件
Config::Config(const std::string &conf_file_path) : conf_file_path_(conf_file_path) {
    xml_file_ = new TiXmlDocument();
    bool rt = xml_file_->LoadFile(conf_file_path);  // 加载XML文件
    if (!rt) {
        std::cout << std::format(
                         "start delta error! read conf file [{}] error info: [{}], errorid: [{}], error_row_column:[{} "
                         "row {} column]",
                         conf_file_path, xml_file_->ErrorDesc(), xml_file_->ErrorId(), xml_file_->ErrorRow(),
                         xml_file_->ErrorCol())
                  << std::endl;
        exit(0);
    }
}

// 按顺序读取所有配置项并初始化相应组件
void Config::readConf() {
    TiXmlElement *root = xml_file_->RootElement();

    // 获取<log>结点并调用readLogConfig()
    readLogConfig(root->FirstChildElement("log"));
    readWalConfig(root->FirstChildElement("wal"));
}

// 读取日志配置并初始化日志系统
void Config::readLogConfig(TiXmlElement *log_node) {
    if (!log_node) {
        std::cout << std::format("start delta error! read config file [{}] error, cannot read [log] xml node",
                                 conf_file_path_)
                  << std::endl;
        exit(0);
    }

    TiXmlElement *node = log_node->FirstChildElement("log_path");
    checkConf(node, "log_path");
    log_path = std::string(node->GetText());

    node = log_node->FirstChildElement("log_base_name");
    checkConf(node, "log_base_name");
    log_base_name = std::string(node->GetText());

    node = log_node->FirstChildElement("log_max_file_size");
    checkConf(node, "log_max_file_size");
    int log_max_size = std::atoi(node->GetText());
    log_max_file_size = log_max_size * 1024 * 1024;

    node = log_node->FirstChildElement("log_max_file_num");
    checkConf(node, "log_max_file_num");
    log_max_file_num = std::atoi(node->GetText());
}

// 读取预写日志配置并初始化预写日志日志系统
void Config::readWalConfig(TiXmlElement *wal_node) {
    if (!wal_node) {
        std::cout << std::format("start delta error! read config file [{}] error, cannot read [wal] xml node",
                                 conf_file_path_)
                  << std::endl;
        exit(0);
    }

    TiXmlElement *node = wal_node->FirstChildElement("wal_path");
    checkConf(node, "wal_path");
    wal_path = std::string(node->GetText());

    node = wal_node->FirstChildElement("wal_base_name");
    checkConf(node, "wal_base_name");
    wal_base_name = std::string(node->GetText());

    node = wal_node->FirstChildElement("wal_max_file_size");
    checkConf(node, "wal_max_file_size");
    int wal_max_size = std::atoi(node->GetText());
    wal_max_file_size = wal_max_size * 1024 * 1024;
}

Config::~Config() {
    if (xml_file_) {
        delete xml_file_;
        xml_file_ = NULL;
    }
}

// 用于获取根节点下的指定子节点。
TiXmlElement *Config::getXmlNode(const std::string &name) {
    return xml_file_->RootElement()->FirstChildElement(name.c_str());
}

void Config::checkConf(TiXmlElement *node, std::string conf_option) {
    if (!node || !node->GetText()) {
        std::cout << std::format("start delta error! read config file [{}] error, cannot read [{}] xml node",
                                 conf_file_path_, conf_option)
                  << std::endl;
        exit(0);
    }
}

}  // namespace delta