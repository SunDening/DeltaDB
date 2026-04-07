#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "db.h"
#include "db_impl.h"
#include "iterator.h"

using namespace delta;

// ============================================================================
// 帮助信息
// ./build/bin/deltadb              # 默认路径 ./output/delta
// ./build/bin/deltadb --db /path   # 自定义数据库路径
// ============================================================================

static void PrintHelp() {
    std::cout << R"(DeltaDB CLI - 命令行客户端

命令列表:
  put <key> <value>           写入键值对
  get <key>                   读取键值
  del <key>                   删除键
  scan [begin] [end]          范围扫描（不包含 end），省略参数则扫描全部
  compact [begin] [end]       手动压缩指定范围，省略则压缩全部
  stats                       显示数据库统计信息
  dbdir                       显示当前数据库路径
  help                        显示此帮助信息
  quit / exit                 退出客户端
)";
}

// ============================================================================
// 命令解析
// ============================================================================

struct Command {
    std::string cmd;
    std::vector<std::string> args;
};

static Command ParseCommand(const std::string& line) {
    Command result;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        if (result.cmd.empty()) {
            result.cmd = token;
        } else {
            result.args.push_back(token);
        }
    }
    return result;
}

// ============================================================================
// 各命令处理
// ============================================================================

static void CmdPut(DB* db, const Command& cmd) {
    if (cmd.args.size() < 2) {
        std::cerr << "Usage: put <key> <value>\n";
        return;
    }
    // 合并所有value参数（支持包含空格的值）
    std::string value;
    for (size_t i = 1; i < cmd.args.size(); i++) {
        if (i > 1) value += " ";
        value += cmd.args[i];
    }
    Status s = db->Put(WriteOptions(), cmd.args[0], value);
    if (s.ok()) {
        std::cout << "OK\n";
    } else {
        std::cerr << "Error: " << s.ToString() << "\n";
    }
}

static void CmdGet(DB* db, const Command& cmd) {
    if (cmd.args.empty()) {
        std::cerr << "Usage: get <key>\n";
        return;
    }
    std::string value;
    Status s = db->Get(ReadOptions(), cmd.args[0], &value);
    if (s.ok()) {
        std::cout << value << "\n";
    } else {
        std::cerr << "Error: " << s.ToString() << "\n";
    }
}

static void CmdDel(DB* db, const Command& cmd) {
    if (cmd.args.empty()) {
        std::cerr << "Usage: del <key>\n";
        return;
    }
    Status s = db->Delete(WriteOptions(), cmd.args[0]);
    if (s.ok()) {
        std::cout << "OK\n";
    } else {
        std::cerr << "Error: " << s.ToString() << "\n";
    }
}

static void CmdScan(DB* db, const Command& cmd) {
    ReadOptions opts;
    Iterator* it = db->NewIterator(opts);

    int count = 0;
    if (!cmd.args.empty()) {
        it->Seek(cmd.args[0]);
    } else {
        it->SeekToFirst();
    }

    for (; it->Valid(); it->Next()) {
        if (cmd.args.size() >= 2 && cmd.args[1] <= it->key()) {
            break;
        }
        std::cout << it->key() << " => " << it->value() << "\n";
        count++;
    }

    if (!it->status().ok()) {
        std::cerr << "Scan error: " << it->status().ToString() << "\n";
    } else {
        std::cout << count << " row(s) returned.\n";
    }
    delete it;
}

static void CmdCompact(DB* db, const Command& cmd) {
    if (cmd.args.size() >= 2) {
        std::string_view range[2] = {cmd.args[0], cmd.args[1]};
        db->CompactRange(&range[0], &range[1]);
    } else {
        db->CompactRange(nullptr, nullptr);
    }
    std::cout << "Compaction scheduled.\n";
}

static void CmdStats(DB* db, const Command& cmd) {
    (void)cmd;
    std::string value;
    if (db->GetProperty("delta.stats", &value)) {
        std::cout << value << "\n";
    } else {
        std::cerr << "Failed to get stats.\n";
    }
}

// ============================================================================
// REPL 主循环
// ============================================================================

static void REPL(DB* db) {
    std::cout << "DeltaDB CLI v1.23  (type 'help' for commands)\n\n";

    std::string line;
    while (true) {
        std::cout << "deltadb> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;  // EOF
        }

        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);

        if (line.empty() || line[0] == '#') continue;

        Command cmd = ParseCommand(line);

        if (cmd.cmd == "quit" || cmd.cmd == "exit") {
            break;
        } else if (cmd.cmd == "help" || cmd.cmd == "h" || cmd.cmd == "?") {
            PrintHelp();
        } else if (cmd.cmd == "put") {
            CmdPut(db, cmd);
        } else if (cmd.cmd == "get") {
            CmdGet(db, cmd);
        } else if (cmd.cmd == "del" || cmd.cmd == "delete") {
            CmdDel(db, cmd);
        } else if (cmd.cmd == "scan") {
            CmdScan(db, cmd);
        } else if (cmd.cmd == "compact") {
            CmdCompact(db, cmd);
        } else if (cmd.cmd == "stats") {
            CmdStats(db, cmd);
        } else if (cmd.cmd == "dbdir") {
            std::cout << "Database directory: ./output/delta\n";
        } else {
            std::cerr << "Unknown command: " << cmd.cmd << "  (type 'help' for help)\n";
        }
    }
}

// ============================================================================
// 入口
// ============================================================================

int main(int argc, char* argv[]) {
    const char* db_path = "./output/delta";

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: deltadb [--db <path>] [--help]\n"
                      << "  --db <path>   Database directory (default: ./output/delta)\n"
                      << "  --help        Show this help\n";
            return 0;
        }
    }

    // 1. 初始化全局配置和日志
    start();
    gDBConfig->create_if_missing = true;

    // 2. 打开数据库
    DB* db = nullptr;
    Status s = DB::Open(db_path, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open database at '" << db_path << "': " << s.ToString() << "\n";
        return 1;
    }

    // 3. 进入 REPL 交互
    REPL(db);

    // 4. 关闭数据库
    delete db;
    return 0;
}
