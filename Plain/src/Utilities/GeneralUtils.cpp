#include "pch.h"
#include "GeneralUtils.h"

/*
=========
vectorContains
=========
*/
bool vectorContains(const std::vector<uint32_t>& vector, const uint32_t index) {
    return std::find(vector.begin(), vector.end(), index) != vector.end();
}

/*
=========
dataToCharArray
=========
*/
std::vector<char> dataToCharArray(void* data, const size_t size) {
    std::vector<char> result(size);
    memcpy(result.data(), data, size);
    return result;
}