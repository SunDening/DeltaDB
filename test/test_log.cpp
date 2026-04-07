#include <gtest/gtest.h>
#include <memory>

#include "config.h"
#include "db_impl.h"
#include "log.h"
#include "util.h"

using namespace delta;

TEST(LoggerTest, Init) {
    delta::start();
    DebugLog << "this is a debug log";
    InfoLog << "this is a info log";
    ErrorLog << "this is a error log";
}
