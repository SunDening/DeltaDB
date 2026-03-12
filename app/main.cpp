#include <filesystem>

#include "config.h"
#include "kv_cli.h"
// #include "../src/kv_improve/headers/Little_kv.h"
// #include "../src/kv_improve/headers/WAL.h"

using namespace std;

int main() {
    // 使用配置文件初始化参数
    ConfigLoader loader("./conf/config.json");
    KVConfig conf = loader.getKVConfig();

    filesystem::create_directories(conf.wal_dir);
    filesystem::create_directories(conf.log_dir);
    filesystem::create_directories(conf.sst_dir);

    WAL wal(conf.wal_dir, conf.log_dir, (conf.memtable_threshold / conf.per_kv_size) * 2);
    Little_kv kv(conf.sst_dir, conf.memtable_threshold, conf.compact_threshold, conf.max_file_size, conf.per_kv_size,
                 wal);

    cout << "=== ===  LITTLE_KV BOOT  === ===" << endl << endl;

    // 创建对象 end

    cout << "<<========= input 'cli' to boot cli =========>>" << endl;
    cout << "<<========= input others to end usage =========>>" << endl;

    while (true) {
        cout << "Your input: ";
        string input;
        cin >> input;
        if (input == "cli") {
            cin.ignore(numeric_limits<streamsize>::max(), '\n');  // 清空残留内容
            boot(kv);
        } else if (input == "exit") {
            cout << "=== ===  LITTLE_KV will exit  === ===" << endl;
            break;
        }
    }
}
