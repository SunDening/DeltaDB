#include <gtest/gtest.h>
#include <memory>

#include <deltadb/db/db_impl.h>
#include <deltadb/utils/config.h>
#include <deltadb/utils/log.h>
#include <deltadb/utils/util.h>

using namespace delta;

static void start() {
    // Try multiple config paths: project root first, then parent directory
    const char* config_paths[] = {"./conf/config.xml", "../conf/config.xml", "../../conf/config.xml"};
    const char* chosen_path = config_paths[0];
    for (const char* path : config_paths) {
        if (access(path, F_OK) == 0) {
            chosen_path = path;
            break;
        }
    }
    gDBConfig = std::make_shared<Config>(chosen_path);
    gDBLogger = std::make_shared<Logger>();
    gDBLogger->start();
}

TEST(LoggerTest, Init) {
    start();
    DebugLog << "this is a debug log";
    InfoLog << "this is a info log";
    ErrorLog << "this is a error log";
}
