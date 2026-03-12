#include "kv_cli.h"

using namespace std;

bool boot(Little_kv& kv) {
    cout << "cli: welcome !\n";
    cout << "U shall input get/put/del, or exit to end cli\n";

    while (true) {
        cout << ">>> ";
        string line;
        if (!getline(cin, line)) { // 处理EOF
            cout << "\ncli: detected EOF, exiting...\n";
            break;
        }

        // 去除首尾空格
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        if (line.empty())
            continue; // 跳过空输入

        istringstream iss(line);
        string cmd;
        iss >> cmd;

        try { // 异常处理
            if (cmd == "get") {
                string key;
                if (!(iss >> key)) {
                    cout << "Error: missing key\n";
                    continue;
                }
                cout << kv.get(key) << endl;
            } else if (cmd == "put") {
                string key;
                iss >> key;
                string value;
                getline(iss >> ws, value); // 读取剩余部分作为value（允许空格）

                if (key.empty() || value.empty()) {
                    cout << "Error: missing key or value\n";
                    continue;
                }
                cout << (kv.put(key, value) ? "PUT SUCCESS ovo\n" : "PUT FAILED o^o\n");
            } else if (cmd == "del") {
                string key;
                if (!(iss >> key)) {
                    cout << "Error: missing key\n";
                    continue;
                }
                cout << (kv.del(key) ? "DEL SUCCESS ovo\n" : "DEL FAILED o^o\n");
            } else if (cmd == "putbat") {
                int num;
                if (!(iss >> num)) {
                    cout << "Error: missing bat num\n";
                    continue;
                }
                kv.putbat(num);
            } else if (cmd == "compact") {
                kv.detect_and_schedule();
            } else if (cmd == "log") {
                kv.refresh_log();
            } else if (cmd == "cls") {
                kv.clear();
            } else if (cmd == "quit") {
                cout << "cli: bye bye !\n";
                break;
            } else if (cmd == "help") {
                cout << "Commands:\n"
                     << "  get <key>\n"
                     << "  put <key> <value>\n"
                     << "  putbat <num>\n"
                     << "  del <key>\n"
                     << "  compact\n"
                     << "  log\n"
                     << "  cls\n"
                     << "  quit\n"
                     << "  help\n";
            } else {
                cout << "Error: unknown command (try 'help')\n";
            }
        } catch (const exception &e) {
            cout << "Error: " << e.what() << endl;
        }
    }
    return true;
}
