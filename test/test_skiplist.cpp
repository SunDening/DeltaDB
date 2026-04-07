#include <gtest/gtest.h>

#include "comparator.h"
#include "db_impl.h"
#include "skiplist.h"

typedef std::string Key;

struct Comparator {
    int operator()(const Key& a, const Key& b) const {
        if (a < b) {
            return -1;
        } else if (a > b) {
            return +1;
        } else {
            return 0;
        }
    }
};

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
    delta::gDBConfig = std::make_shared<delta::Config>(chosen_path);
    delta::gDBLogger = std::make_shared<delta::Logger>();
    delta::gDBLogger->start();
}

TEST(SkipListTest, test1) {
    std::cout << "=== SkipList Test ===" << std::endl;
    start();

    delta::Arena arena;
    Comparator cmp;
    std::cout << "init skiplist begin" << std::endl;
    delta::SkipList<std::string, Comparator> skiplist(cmp, &arena);
    std::cout << "init skiplist success" << std::endl;
    skiplist.Insert("abc");
    skiplist.Insert("def");
    skiplist.Insert("ghi");
    skiplist.Contains("abc") ? std::cout << "contains abc" << std::endl
                             : std::cout << "does not contain abc" << std::endl;
    skiplist.Contains("def") ? std::cout << "contains def" << std::endl
                             : std::cout << "does not contain def" << std::endl;
    skiplist.Contains("ghi") ? std::cout << "contains ghi" << std::endl
                             : std::cout << "does not contain ghi" << std::endl;
    skiplist.Contains("adc") ? std::cout << "contains adc" << std::endl
                             : std::cout << "does not contain adc" << std::endl;
}
