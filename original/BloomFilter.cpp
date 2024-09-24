// BloomFilter.cpp
#include "BloomFilter.h"

BloomFilter::BloomFilter(int size, int numHashes) : size(size), numHashes(numHashes), bitArray(size, false) {}

int BloomFilter::hash1(int key) const {
    return key % size;
}

int BloomFilter::hash2(int key) const {
    return (key * 7 + 13) % size;
}

void BloomFilter::add(int key) {
    for(int i = 0; i < numHashes; ++i){
        int combinedHash = (hash1(key) + i * hash2(key)) % size;
        bitArray[combinedHash] = true;
    }
}

bool BloomFilter::possiblyContains(int key) const {
    for(int i = 0; i < numHashes; ++i){
        int combinedHash = (hash1(key) + i * hash2(key)) % size;
        if(!bitArray[combinedHash]) return false;
    }
    return true;
}