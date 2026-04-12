#pragma once

namespace delta {

/**
 * 用于将多个有序迭代器合并成一个统一的迭代器（多路归并）。
 */

class Comparator;
class Iterator;

/**
 * @brief 接收 n 个子迭代器，返回一个合并后的迭代器
 * 按 key 的顺序（使用 Comparator 比较）依次遍历所欲子迭代器中的数据
 * 不做去重：如果同一个 key 在 K 个子迭代器中存在，会被输出 K 次
 */
Iterator* NewMergeIterator(const Comparator* comparator, Iterator** children, int n);

}