// BloomFilter.h
#ifndef BLOOMFILTER_H
#define BLOOMFILTER_H

#include <vector>

class BloomFilter {
private:
    std::vector<bool> bitArray;
    int size;
    int numHashes;

public:
    BloomFilter(int size, int numHashes);
    ~BloomFilter() = default;

    int hash1(int key) const;
    int hash2(int key) const;

    void add(int key);
    bool possiblyContains(int key) const;
};

#endif // BLOOMFILTER_H