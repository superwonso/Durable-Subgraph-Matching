#ifndef BLOOMFILTER_H
#define BLOOMFILTER_H

#include <vector>
#include <string>
#include <functional>

using namespace std;

class BloomFilter {
public:
    // Constructor to initialize Bloom filter with given size and number of hash functions
    BloomFilter(size_t size, size_t hashCount);

    // Method to add an item to the Bloom filter
    void add(const string& item);

    // Method to check if an item is possibly in the Bloom filter
    bool possiblyContains(const string& item) const;

private:
    vector<bool> bitArray;  // Bit array to store Bloom filter bits
    size_t size;            // Size of the bit array
    size_t hashCount;       // Number of hash functions

    // Private method to generate a hash value for an item with a given seed
    size_t hash(const string& item, size_t seed) const;
};

#endif // BLOOMFILTER_H
