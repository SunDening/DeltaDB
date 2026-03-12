#include "kv_core.h"

Little_kv::Little_kv(const string &sst_dir_path,
                     const size_t memtable_threshold,
                     const size_t compact_threshold, const size_t max_size, const size_t per_kv_size,
                     WAL& wal)
    : sst_dir_path(sst_dir_path),
      memtable_size_threshold(memtable_threshold),
      compact_threshold(compact_threshold), max_file_size(max_size), per_kv_size(per_kv_size), wal(wal) {
    // cout << "========= MemTable init Begin ! =========" << endl;
    wal.log("[Little_kv] : ========= MemTable init Begin ! =========");
    active_memtable = make_shared<MemTable>();
    // immutable_memtable = nullptr;
    sst_manager = make_shared<SSTableManager>(sst_dir_path, wal, max_file_size, 4, per_kv_size);
    mem_manager = make_shared<MemTableManager>();

    Init_data();  // recover last data from wal

    sst_manager->load(); // load sst files to manifest
    // (*manifest).startBackgroundSave();  // 后台线程定时持久化manifest

    flush_thread = thread(&Little_kv::flush_background, this);
    
    // compact 相关
    compaction_mgr.set_compact_executor([this](CompactionTask task) {
        CompactLn(task.target_level);  // 执行compact任务
    });
    compaction_mgr.start();  // 初始化条件变量并启动compact工作线程
    detector_thread = thread(&Little_kv::detector_loop, this);  // 启动检测线程
    
    // cout << "========= MemTable init Success ! =========" << endl;
    wal.log("[Little_kv] : ========= MemTable init Success ! =========");
}

Little_kv::~Little_kv() {
    // 终止flush
    {
        lock_guard<mutex> flush_lock(flush_mutex);
        stop_flush = true;
    }
    flush_cv.notify_one();  // 通知（唤醒）一个正在wait()的线程，只发信号，不等待
    flush_thread.join();  // 主线程在这里卡住，直到flush_thread的函数体结束才继续往下执行

    // 终止compact
    stop_detector = true;
    if (detector_thread.joinable()) detector_thread.join();
    compaction_mgr.stop();  // 终止compact工作线程

    // 持久化manifest
    {
        unique_lock<shared_mutex> rw_manifest_lock(rw_manifest_mutex);
        sst_manager->save(); // // write sst files information to manifest file
        // (*manifest).stopBackgroundSave();  // 关闭后台执行定时任务的线程
    }
    // cout << "========= OVER ! =========" << endl;
    wal.log("[~Little_kv] : ========= OVER ! =========");
}

void Little_kv::Init_data() {
    wal.recover_unflushed(mem_manager);
    wal.recover_current(active_memtable);  // 恢复当前活跃的wal_current.log到active_mem
}

void Little_kv::Flush() {
    cout << "[Little_kv::Flush()] : 进入Flush()" << endl;
    while ((*mem_manager).has_immutable()) {
        // 准备生成 SST 文件
        int level = 0;
        int next_sst_id = sst_manager->getNextSstId(level);
        string next_sst_name = sst_manager->getNextSstName(level, next_sst_id);
        string full_sst_name = sst_dir_path + next_sst_name;
        ofstream sst_out(full_sst_name, ios::trunc);
        if (!sst_out.is_open()) {
            cerr << "Failed to open " << full_sst_name << endl;
            continue;
        }

        // 遍历 MemTable，写入文件并更新 Bloom filter
        string smallest_k, largest_k;
        size_t file_size = 0;
        BloomFilter filter(sst_manager->allow_file_sizes[level] / per_kv_size, 0.01);
        auto oldest_immutable_entry = (*mem_manager).get_oldest_immutable();
        auto entries = oldest_immutable_entry.mem->traverse();  // 遍历immu_table中的数据
        if (entries.empty()) {
            wal.log("[Little_kv::Flush] Skip empty MemTable from " + oldest_immutable_entry.wal_filename);
            wal.remove_wal(oldest_immutable_entry.wal_filename);
            (*mem_manager).pop_oldest_immutable();
            continue;
        }

        for (const auto &kv : entries) {
            sst_out << kv.first << " " << kv.second << "\n";
            filter.add(kv.first);
            file_size += kv.first.size() + kv.second.size();
        }
        sst_out.close();

        if (!entries.empty()) {
            smallest_k = entries.front().first;
            largest_k = entries.back().first;
        }

        // 构造元数据并写入 Manifest
        cout << "[Little_kv::Flush()] : 正在构造元数据fmd" << endl;
        FileMetaData fmd(level, next_sst_id, next_sst_name, smallest_k, largest_k, file_size, filter);
        sst_manager->addFile(fmd);

        wal.remove_wal(oldest_immutable_entry.wal_filename);  // 从 wal_manifest 中移除并删除文件
        (*mem_manager).pop_oldest_immutable();  // 从归档管理队列中移除

        // 打日志
        wal.log("[Little_kv::Flush] Flushed " + to_string(entries.size()) + 
        " kv pairs from " + oldest_immutable_entry.wal_filename + 
        " to " + next_sst_name);
    }
    sst_manager->save();  // 更新sst manifest
}


// 主动触发 + 定时机制
void Little_kv::flush_background() {
    while (true) {
        std::unique_lock<std::mutex> flush_lock(flush_mutex);
        flush_cv.wait_for(flush_lock, std::chrono::seconds(3), [this] {
            return flush_needed || stop_flush;
        });

        if (stop_flush) break;
        if (flush_needed) {
            Flush();  // 刷盘逻辑
            flush_needed = false;
        }
    }
}

// 尝试主动触发刷盘（通过条件变量），尝试指不一定会触发
void Little_kv::maybe_trigger_flush() {
    {
        lock_guard<mutex> flush_lock(flush_mutex);
        flush_needed = true;
    }
    flush_cv.notify_one();
}


bool Little_kv::CompactLn(int target_level) {

    FileMetaData source_file = sst_manager->getEarliestFileByLevel(target_level - 1);

    // 将target_files、source_file中的数据读入内存，按照由旧到新的顺序读入以覆盖旧值，并按照key排序
    unordered_map<string, string> data_map;
    vector<pair<string, string>> merged_data;

    // 找出key序列，并查找相关的target_files
    set<string> key_set = sst_manager->getKeySet(source_file);
    vector<FileMetaData> overlap_files = sst_manager->getRelativeSsts(target_level, key_set);

    // map方便插入和覆盖旧值
    sst_manager->readFileToMap(overlap_files, data_map); // target_files中可能为空，更可能有数据
    sst_manager->readFileToMap(source_file, data_map);

    // 将map数据存入 vector<pair<string, string>>，以方便对key排序
    for (const auto &[k, v] : data_map) {
        merged_data.push_back(make_pair(k, v));
    }
    // 对set中的数据按key排序，从小到大
    sort(merged_data.begin(), merged_data.end(),
         [](const pair<string, string> &p1, const pair<string, string> &p2) {
             return p1.first <= p2.first;
         });

    // 根据目标文件level，确定预计key的数量
    size_t expect_key_num = sst_manager->allow_file_sizes[target_level] / per_kv_size;

    bool last_compact = (target_level == 3);  // 是否是向最底层的合并

    // 根据target_file_size划分merged_data并持久化到target_files
    size_t current_chunk_size = 0;
    unordered_map<string, string>
        current_chunk; // 临时存储每个target_file的数据
    vector<FileMetaData> new_target_files;

    string target_fnames = "{";

    for (int i = 0; i < merged_data.size(); i++) {
        // 仅在最底层处理删除标记
        if (last_compact && (merged_data[i].second == TOMBSTONE))
            continue;

        current_chunk[merged_data[i].first] = merged_data[i].second;
        current_chunk_size += (merged_data[i].first.size() + merged_data[i].second.size());

        if (current_chunk_size >= sst_manager->allow_file_sizes[target_level]) {  // 触发合并
            int id = sst_manager->getNextSstId(target_level);
            string filename = sst_manager->getNextSstName(target_level, id);
            string s_k = "z", l_k = "a";
            if ((!last_compact) && sst_manager->getKeyRange_contains_del(current_chunk, s_k,
                                                          l_k)) { // 中间层合并
                BloomFilter bf(expect_key_num, 0.01);
                FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
                // 将current_chunk的key添加到fmd的filter
                for (const auto& [key, value] : current_chunk) {
                    fmd.file_size += (key.size() + value.size());
                    fmd.filter.add(key);
                }
                sst_manager->addFile(fmd);
                // write data from map to sst_file
                sst_manager->writeMapToSst(current_chunk, filename);
                target_fnames += filename + " ";
            } else if (last_compact &&
                       sst_manager->getKeyRange_ignore_del(current_chunk, s_k,
                                              l_k)) { // 最底层合并
                BloomFilter bf(expect_key_num, 0.01);
                FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
                // 将current_chunk的key添加到fmd的filter
                for (const auto& [key, value] : current_chunk) {
                    fmd.file_size += (key.size() + value.size());
                    fmd.filter.add(key);
                }
                sst_manager->addFile(fmd);
                // write data from map to sst_file
                sst_manager->writeMapToSst(current_chunk, filename);
                target_fnames += filename + " ";
            } else {
                cerr << "合并到 "<< target_level <<" 出错！" << endl;
                return false;
            }
            current_chunk.clear();
            current_chunk_size = 0;
        }
    }
    // 写入残余数据
    if (!current_chunk.empty()) {
        int id = sst_manager->getNextSstId(target_level);
        string filename = sst_manager->getNextSstName(target_level, id);
        string s_k = "z", l_k = "a";
        if ((!last_compact) &&
            sst_manager->getKeyRange_contains_del(current_chunk, s_k, l_k)) { // 中间层合并
            BloomFilter bf(expect_key_num, 0.01);
            FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
            // 将current_chunk的key添加到fmd的filter
            for (const auto& [key, value] : current_chunk) {
                fmd.file_size += (key.size() + value.size());
                fmd.filter.add(key);
            }
            sst_manager->addFile(fmd);
            // write data from map to sst_file
            sst_manager->writeMapToSst(current_chunk, filename);
            target_fnames += filename + " ";
        } else if (last_compact && sst_manager->getKeyRange_ignore_del(current_chunk, s_k, l_k)) { // 最底层合并
            BloomFilter bf(expect_key_num, 0.01);
            FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
            for (const auto& [key, value] : current_chunk) {
                fmd.file_size += (key.size() + value.size());
                fmd.filter.add(key);
            }
            sst_manager->addFile(fmd);
            // write data from map to sst_file
            sst_manager->writeMapToSst(current_chunk, filename);
            target_fnames += filename + " ";
        } else {
            cerr << "合并出错！" << endl;
            return false;
        }
    }

    // 记录日志
    string msg = "[CompactLn] : compacting {" + source_file.filename + "} and {";
    for (auto& fmd : overlap_files) {
        msg += fmd.filename + " ";
    }
    msg += "} to be ";
    target_fnames += "}";
    msg += target_fnames;
    wal.log(msg);

    // 删除源数据文件和相关原target文件
    RemoveSstFiles(source_file);
    RemoveSstFiles(overlap_files);

    // 保险起见，持久化manifest
    sst_manager->save();

    return true;
}

// 实际的合并过程 -- 按批合并
// 注意：调用前必须先持有 manifest 的写锁！
bool Little_kv::CompactLnBat(int target_level) {

    vector<FileMetaData> source_files = sst_manager->getFilesByLevel(target_level - 1);

    // 将target_files、source_files中的数据读入内存，按照由旧到新的顺序读入以覆盖旧值，并按照key排序
    unordered_map<string, string> data_map;
    vector<pair<string, string>> merged_data;

    // 找出key序列，并查找相关的target_files
    set<string> key_set = sst_manager->getKeySet(source_files);
    vector<FileMetaData> overlap_files = sst_manager->getRelativeSsts(target_level, key_set);

    // map方便插入和覆盖旧值
    sst_manager->readFileToMap(overlap_files, data_map); // target_files中可能为空，更可能有数据
    sst_manager->readFileToMap(source_files, data_map);
    
    // 将map数据存入 vector<pair<string, string>>，以方便对key排序
    for (const auto &[k, v] : data_map) {
        merged_data.push_back(make_pair(k, v));
    }
    // 对set中的数据按key排序，从小到大
    sort(merged_data.begin(), merged_data.end(),
         [](const pair<string, string> &p1, const pair<string, string> &p2) {
             return p1.first <= p2.first;
         });

    // 根据目标文件level，确定预计key的数量
    size_t expect_key_num = sst_manager->allow_file_sizes[target_level] / per_kv_size;

    bool last_compact = (target_level == 3);  // 是否是向最底层的合并

    // 根据target_file_size划分merged_data并持久化到target_files
    size_t current_chunk_size = 0;
    unordered_map<string, string>
        current_chunk; // 临时存储每个target_file的数据
    vector<FileMetaData> new_target_files;

    for (int i = 0; i < merged_data.size(); i++) {
        // 仅在最底层处理删除标记
        if (last_compact && (merged_data[i].second == TOMBSTONE))
            continue;

        current_chunk[merged_data[i].first] = merged_data[i].second;
        current_chunk_size += (merged_data[i].first.size() + merged_data[i].second.size());

        if (current_chunk_size >= sst_manager->allow_file_sizes[target_level]) {  // 触发合并
            int id = sst_manager->getNextSstId(target_level);
            string filename = sst_manager->getNextSstName(target_level, id);
            string s_k = "z", l_k = "a";
            if ((!last_compact) && sst_manager->getKeyRange_contains_del(current_chunk, s_k,
                                                          l_k)) { // 中间层合并
                BloomFilter bf(expect_key_num, 0.01);
                FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
                // 将current_chunk的key添加到fmd的filter
                for (const auto& [key, value] : current_chunk) {
                    fmd.file_size += (key.size() + value.size());
                    fmd.filter.add(key);
                }
                sst_manager->addFile(fmd);
                // write data from map to sst_file
                sst_manager->writeMapToSst(current_chunk, filename);
            } else if (last_compact &&
                       sst_manager->getKeyRange_ignore_del(current_chunk, s_k,
                                              l_k)) { // 最底层合并
                BloomFilter bf(expect_key_num, 0.01);
                FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
                // 将current_chunk的key添加到fmd的filter
                for (const auto& [key, value] : current_chunk) {
                    fmd.file_size += (key.size() + value.size());
                    fmd.filter.add(key);
                }
                sst_manager->addFile(fmd);
                // write data from map to sst_file
                sst_manager->writeMapToSst(current_chunk, filename);
            } else {
                cerr << "合并到 "<< target_level <<" 出错！" << endl;
                return false;
            }
            current_chunk.clear();
            current_chunk_size = 0;
        }
    }
    // 写入残余数据
    if (!current_chunk.empty()) {
        int id = sst_manager->getNextSstId(target_level);
        string filename = sst_manager->getNextSstName(target_level, id);
        string s_k = "z", l_k = "a";
        if ((!last_compact) &&
            sst_manager->getKeyRange_contains_del(current_chunk, s_k, l_k)) { // 中间层合并
            BloomFilter bf(expect_key_num, 0.01);
            FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
            // 将current_chunk的key添加到fmd的filter
            for (const auto& [key, value] : current_chunk) {
                fmd.file_size += (key.size() + value.size());
                fmd.filter.add(key);
            }
            sst_manager->addFile(fmd);
            // write data from map to sst_file
            sst_manager->writeMapToSst(current_chunk, filename);
        } else if (last_compact && sst_manager->getKeyRange_ignore_del(current_chunk, s_k, l_k)) { // 最底层合并
            BloomFilter bf(expect_key_num, 0.01);
            FileMetaData fmd(target_level, id, filename, s_k, l_k, 0, bf);
            for (const auto& [key, value] : current_chunk) {
                fmd.file_size += (key.size() + value.size());
                fmd.filter.add(key);
            }
            sst_manager->addFile(fmd);
            // write data from map to sst_file
            sst_manager->writeMapToSst(current_chunk, filename);
        } else {
            cerr << "合并出错！" << endl;
            return false;
        }
    }

    // 删除源数据文件和相关原target文件
    RemoveSstFiles(source_files);
    RemoveSstFiles(overlap_files);

    return true;
}

void Little_kv::detector_loop() {
    while (!stop_detector) {
        detect_and_schedule();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// 实际上在此处判断是否需要合并
void Little_kv::detect_and_schedule() {
    // 压实分数，L0按文件数算，其它层按容量大小算。
    // 压实分数小于1不合并。压实分数越高优先级越高；压实分数相同，越靠近内存优先级越高。
    // L0的压实分数大于1时，L0无条件优先合并。
    double compact_score = 0.0;  
    int target_level = 0;

    {  // 先用读锁，只读元数据
        shared_lock<std::shared_mutex> manifest_read_lock(rw_manifest_mutex);

        // L0的压实分数
        compact_score = (double)sst_manager->getFilesByLevel(0).size() / (double)compact_threshold;
        if (compact_score >= 1) {
            target_level = 1;
        } else {
            double tmp_max_score = 0;
            for (int level=1;level <= 2; level++) {
                compact_score = (double)sst_manager->getLevelSize(level) / (double)(sst_manager->allow_file_sizes[level]*sst_manager->levels_multiple);
                if (compact_score > tmp_max_score) {
                    tmp_max_score = compact_score;
                    target_level = level + 1;
                }
            }
            compact_score = tmp_max_score;
        }
    } // 读锁作用域结束，释放

    if (compact_score < 1) {  // 不需要合并
        return;
    }
    
    {  // 需要合并，下面开始需要对 manifest 做写操作，需要写锁
        unique_lock<std::shared_mutex> manifest_write_lock(rw_manifest_mutex);
        compaction_mgr.enqueue_task({target_level});
    }
}

// put --- thread safe
bool Little_kv::put(const std::string &key, const std::string &value) {

    lock_guard<mutex> put_lock(edit_mutex);

    wal.writeWAL("PUT", key, value);

    bool ret = (*active_memtable).insert(key, value);

    if ((*active_memtable).GetMemTableSize() >= memtable_size_threshold) {
        wal.log("[Little_kv::put] : size over, gona to add to flush queue");
        // 当前wal归档，令开一个新的wal，将当前wal加入刷盘队列
        string wal_name = wal.rotate();  // 归档、重开wal
        wal.log("[Little_kv::put] : rotate over");
        shared_ptr<MemTable> immutable_memtable = active_memtable;
        active_memtable = make_shared<MemTable>();
        (*mem_manager).add_immutable(immutable_memtable, wal_name);
        wal.log("[Little_kv::put] : add_immutable over");
        maybe_trigger_flush();  // 尝试主动开启刷盘
    }

    return ret;
}

bool Little_kv::putbat(const int num) {
    lock_guard<mutex> put_lock(edit_mutex);
    unordered_map<string, string> put_buff;
    for (int i=0;i<num;i++) {
        cout << "[" << (i+1) << "/" << num <<"]-> ";
        string line;
        if (!getline(cin, line)) break;
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty()) continue; // 跳过空输入
        istringstream iss(line);
        string cmd;
        iss >> cmd;
        if (cmd == "put") {
            string key;
            iss >> key;
            string value;
            getline(iss >> ws, value); // 读取剩余部分作为value（允许空格）
            if (key.empty() || value.empty()) continue;
            put_buff[key] = value;
        } else if (cmd == "del") {
            string key;
            iss >> key;
            if (key.empty()) continue;
            put_buff[key] = TOMBSTONE;
        } else {
            wal.log("[Little_kv::putbat] : op Error");
            return false;
        }
    }
    bool ret = wal.writeBatWAL(put_buff);
    // 将put_buff中的数据写入wal
    if (ret) {
        lock_guard<mutex> put_lock(edit_mutex);
        for (const auto& [key, value] : put_buff) {
            ret &= (*active_memtable).insert(key, value);
        }
        if ((*active_memtable).GetMemTableSize() >= memtable_size_threshold) {
            // 当前wal归档，令开一个新的wal，将当前wal加入刷盘队列
            string wal_name = wal.rotate();  // 归档、重开wal
            shared_ptr<MemTable> immutable_memtable = active_memtable;
            active_memtable = make_shared<MemTable>();
            (*mem_manager).add_immutable(immutable_memtable, wal_name);
            maybe_trigger_flush();
        }
    }
    return ret;
}

bool Little_kv::del(const std::string &key) {
    return put(key, TOMBSTONE); // 实质上是做了put操作，value为删除标记
}

string Little_kv::get(const string &key) {
    // order : active_memtable immutable_memtable  L0  L1

    // 先查 active_memtable
    optional<string> ret = (*active_memtable).search(key);
    if (ret.has_value()) { // means exists the key
        if (ret == TOMBSTONE)
            return "NOT_FOUND"; // deled tag
        return ret.value();             // find the value
    }

    // 改为从immu_mem队列中查找
    for (const auto& immu_mem : (*mem_manager).get_immutable_list_desc()) {
        ret = (*immu_mem).search(key);
        if (ret.has_value()) { // means exists the key
            if (ret == TOMBSTONE)
                return "NOT_FOUND"; // deled tag
            return ret.value();             // find the value
        }
    }

    // 查各level文件，从顶层到底层：L0 -> L3，即由新到旧
    shared_lock<shared_mutex> manifest_lock(rw_manifest_mutex);

    // search from L0 files
    vector<FileMetaData> l0_files = sst_manager->getFilesByLevel(0);
    // L0比较特殊，不同的L0中key可能重复，因此要按照id由大到小的顺序查找，以符合数据由新到旧的顺序
    // 且L0的value没有经过压缩，可直接返回
    sort(l0_files.begin(), l0_files.end(),
         [](const FileMetaData &f1, const FileMetaData &f2) {
             return f1.id > f2.id;
         });
    for (const auto &file : l0_files) {
        // 初步定位key范围
        if ((key < file.smallest_key) || (key > file.largest_key)) {
            wal.log("[Little_kv::get L0] : key 范围淘汰：" + key + ", smallest_key:" + file.smallest_key + ", largest_key:" + file.largest_key);
            continue;
        }
        // 使用布隆过滤器再次筛查目标sst文件是否包含该key
        if (!file.filter.contains(key)) {
            // cout << "Bloom Pass : " << file.filename << endl;
            wal.log("[get " + key + "] : Bloom Pass : " + file.filename);
            continue;
        }
        wal.log("正在使用"+ file.filename +"查找：" + key);
        // 通过以上两道粗粒度的筛查后，才读取磁盘文件
        string full_sst_path = sst_dir_path + file.filename;
        auto fp = fd_cache.get(full_sst_path);
        if (fp && fp->is_open()) {
            string line;
            wal.log(file.filename + "被打开查找：" + key);
            while (getline(*fp, line)) {
                istringstream iss(line);
                string k, v;
                if (iss >> k) {
                    wal.log("key: " + key);
                    getline(iss, v);
                    if (key == k) {
                        v = v.empty() ? "" : v.substr(1);
                        if (v == TOMBSTONE) return "NOT_FOUND";
                        return v;
                    }
                }
            }
        } else {
            wal.log("使用fd_cache打开文件失败！");
        }
    }
    // search from L1、L2、L3 files
    // L1之后的层的value都经过了压缩，需要在searchFromSst中解压缩后比较
    ret = "NOT_FOUND";
    for (int level=1;level <= 3;level++) {
        ret = sst_manager->searchFromSst(level, key, fd_cache);
        if (ret != "NOT_FOUND") break;
    }

    if (ret == TOMBSTONE) ret = "NOT_FOUND";
    return ret.value();
}

void Little_kv::clear() {
    // 记录日志
    wal.log("clear all the data\n");
    // 清除内存中数据
    {
        lock_guard<mutex> put_lock(edit_mutex);
        active_memtable.reset(new MemTable);
    }
    // 清除wal
    wal.clearWAL();
    // 删除sst文件
    {
        unique_lock<std::shared_mutex> manifest_write_lock(rw_manifest_mutex);
        vector<FileMetaData> fmdList = sst_manager->getFileList();
        RemoveSstFiles(fmdList);
    }

    fd_cache.clear();
}








// =================================== 工具性函数 ======================================

bool Little_kv::RemoveSstFiles(FileMetaData &file) {
    remove((sst_dir_path + file.filename).c_str());
    sst_manager->removeFile(file.filename);
    sst_manager->save();
    return true;
}

bool Little_kv::RemoveSstFiles(vector<FileMetaData> &files) {
    for (const auto &file : files) {
        remove((sst_dir_path + file.filename).c_str());
        sst_manager->removeFile(file.filename);
    }
    sst_manager->save();
    return true;
}

void Little_kv::refresh_log() {
    wal.flush_log();
}