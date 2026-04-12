#include <deltadb/table/cache.h>
#include <deltadb/utils/comparator.h>
#include <deltadb/utils/config.h>
#include <deltadb/utils/dbformat.h>

namespace delta {

Config::Config(const std::string &conf_file_path) : fd_limiter(50) {
    conf_file_path_ = conf_file_path;
    xml_file_ = new TiXmlDocument();

    bool rt = xml_file_->LoadFile(conf_file_path);
    if (!rt) {
        std::cout
            << std::format(
                   "start delta error! read conf file [{}] error info: [{}], errorid: [{}], error_row_column:[{} row "
                   "{} column]",
                   conf_file_path, xml_file_->ErrorDesc(), xml_file_->ErrorId(), xml_file_->ErrorRow(),
                   xml_file_->ErrorCol())
            << std::endl;
        exit(0);
    }
    readConf();  // 读取各类配置

    // 初始化比较器
    // comparator 是 User Comparator，用于比较用户键
    // internal_comparator 是 Internal Key Comparator，用于比较 SST 中的内部键（包含序列号）
    static InternalKeyComparator internal_comp(BytewiseComparator());
    comparator = BytewiseComparator();
    internal_comparator = &internal_comp;
}

void Config::readConf() {
    TiXmlElement *root = xml_file_->RootElement();

    // <log>
    TiXmlElement *log_node = root->FirstChildElement("log");
    checkType(log_node, "log");
    readLogConfig(log_node);

    // <db>
    TiXmlElement *db_node = root->FirstChildElement("db");
    checkType(db_node, "db");
    readDBConfig(db_node);
}

void Config::readLogConfig(TiXmlElement *log_node) {
    TiXmlElement *node = log_node->FirstChildElement("log_path");
    checkItem(node, "log_path");
    log_path_ = std::string(node->GetText());

    node = log_node->FirstChildElement("log_prefix");
    checkItem(node, "log_prefix");
    log_prefix_ = std::string(node->GetText());

    node = log_node->FirstChildElement("log_max_file_size");
    checkItem(node, "log_max_file_size");
    log_max_file_size_ = std::atoi(node->GetText()) * 1024 * 1024;

    node = log_node->FirstChildElement("db_log_level");
    checkItem(node, "db_log_level");
    db_log_level_ = stringToLevel(std::string(node->GetText()));

    node = log_node->FirstChildElement("log_sync_interval");
    checkItem(node, "log_sync_interval");
    log_sync_interval_ = std::atoi(node->GetText());
}

void Config::readDBConfig(TiXmlElement *db_node) {
    TiXmlElement *node = db_node->FirstChildElement("db_path");
    checkItem(node, "db_path");
    db_path = std::string(node->GetText());

    node = db_node->FirstChildElement("num_levels");
    checkItem(node, "num_levels");
    num_levels = std::atoi(node->GetText());

    node = db_node->FirstChildElement("l0_compaction_trigger");
    checkItem(node, "l0_compaction_trigger");
    l0_compaction_trigger = std::atoi(node->GetText());

    node = db_node->FirstChildElement("l0_slowdown_writes_trigger");
    checkItem(node, "l0_slowdown_writes_trigger");
    l0_slowdown_writes_trigger = std::atoi(node->GetText());

    node = db_node->FirstChildElement("l0_stop_writes_trigger");
    checkItem(node, "l0_stop_writes_trigger");
    l0_stop_writes_trigger = std::atoi(node->GetText());

    node = db_node->FirstChildElement("max_mem_compact_level");
    checkItem(node, "max_mem_compact_level");
    max_mem_compact_level = std::atoi(node->GetText());

    node = db_node->FirstChildElement("read_bytes_period");
    checkItem(node, "read_bytes_period");
    read_bytes_period = std::atoi(node->GetText());

    node = db_node->FirstChildElement("create_if_missing");
    checkItem(node, "create_if_missing");
    create_if_missing = (std::string(node->GetText()) == "true");

    node = db_node->FirstChildElement("error_if_exists");
    checkItem(node, "error_if_exists");
    error_if_exists = (std::string(node->GetText()) == "true");

    node = db_node->FirstChildElement("paranoid_checks");
    checkItem(node, "paranoid_checks");
    paranoid_checks = (std::string(node->GetText()) == "true");

    node = db_node->FirstChildElement("write_buffer_size");
    checkItem(node, "write_buffer_size");
    write_buffer_size = std::atoi(node->GetText());

    node = db_node->FirstChildElement("max_open_files");
    checkItem(node, "max_open_files");
    max_open_files = std::atoi(node->GetText());

    node = db_node->FirstChildElement("block_size");
    checkItem(node, "block_size");
    block_size = std::atoi(node->GetText());

    node = db_node->FirstChildElement("block_restart_internal");
    checkItem(node, "block_restart_internal");
    block_restart_internal = std::atoi(node->GetText());

    node = db_node->FirstChildElement("max_file_size");
    checkItem(node, "max_file_size");
    max_file_size = std::atoi(node->GetText());

    node = db_node->FirstChildElement("zstd_compression_level");
    checkItem(node, "zstd_compression_level");
    zstd_compression_level = std::atoi(node->GetText());

    node = db_node->FirstChildElement("reuse_logs");
    checkItem(node, "reuse_logs");
    reuse_logs = (std::string(node->GetText()) == "true");
}

void Config::checkType(TiXmlElement *node, std::string type) {
    if (!node) {
        std::cout << std::format("start delta error! read config file [{}] error, cannot read [{}] xml node",
                                 conf_file_path_, type);
        exit(0);
    }
}

void Config::checkItem(TiXmlElement *node, std::string item) {
    if (!node || !node->GetText()) {
        std::cout << std::format("start delta error! read config file [{}] error, cannot read [{}] xml node",
                                 conf_file_path_, item)
                  << std::endl;
        exit(0);
    }
}

// ???????????????
TiXmlElement *Config::getXmlNode(const std::string &name) {
    return xml_file_->RootElement()->FirstChildElement(name.c_str());
}

Config::~Config() {
    if (xml_file_) {
        delete xml_file_;
        xml_file_ = NULL;
    }

    if (this->block_cache != nullptr) {
        delete this->block_cache;
    }
}

}  // namespace delta