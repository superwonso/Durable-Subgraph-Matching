#include <iostream>
#include <vector>
#include <functional>

using namespace std;

class BloomFilter {
public:
    BloomFilter(size_t size, size_t hashCount) : bitArray(size), size(size), hashCount(hashCount) {}

    void add(const string& item) {
        for (size_t i = 0; i < hashCount; ++i) {
            size_t hashValue = hash(item, i);
            bitArray[hashValue % size] = true;
        }
    }

    bool possiblyContains(const string& item) const {
        for (size_t i = 0; i < hashCount; ++i) {
            size_t hashValue = hash(item, i);
            if (!bitArray[hashValue % size]) {
                return false;
            }
        }
        return true;
    }

private:
    vector<bool> bitArray;
    size_t size;
    size_t hashCount;

    size_t hash(const string& item, size_t seed) const {
        return std::hash<string>{}(item) ^ (seed * 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }
};