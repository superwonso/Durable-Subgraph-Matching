#include <iostream>
#include <vector>
#include <functional>
#include "bloom_filter.h"

using namespace std;

// Constructor to initialize Bloom filter with given size and number of hash functions
BloomFilter::BloomFilter(size_t size, size_t hashCount) 
    : bitArray(size), size(size), hashCount(hashCount) {}

// Method to add an item to the Bloom filter
void BloomFilter::add(const string& item) {
    for (size_t i = 0; i < hashCount; ++i) {
        size_t hashValue = hash(item, i);
        bitArray[hashValue % size] = true;
    }
}

// Method to check if an item is possibly in the Bloom filter
bool BloomFilter::possiblyContains(const string& item) const {
    for (size_t i = 0; i < hashCount; ++i) {
        size_t hashValue = hash(item, i);
        if (!bitArray[hashValue % size]) {
            return false;
        }
    }
    return true;
}

// Private method to generate a hash value for an item with a given seed
size_t BloomFilter::hash(const string& item, size_t seed) const {
    return std::hash<string>{}(item) ^ (seed * 0x9e3779b9 + (seed << 6) + (seed >> 2));
}
