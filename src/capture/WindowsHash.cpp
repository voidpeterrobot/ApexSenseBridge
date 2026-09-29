#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "capture/FeedbackRecorder.h"
#include <array>
#include <stdexcept>

namespace asb::capture {
std::string sha256File(const std::filesystem::path& path) {
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Hash() { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    } handle;
    if (BCryptOpenAlgorithmProvider(&handle.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
        BCryptCreateHash(handle.algorithm,&handle.hash,nullptr,0,nullptr,0,0)<0)
        throw std::runtime_error("SHA256 initialization failed");
    std::ifstream stream(path,std::ios::binary);
    if(!stream) throw std::runtime_error("cannot hash file");
    std::array<unsigned char,65536> buffer{};
    while(stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),buffer.size());
        if(stream.gcount() && BCryptHashData(handle.hash,buffer.data(),static_cast<ULONG>(stream.gcount()),0)<0)
            throw std::runtime_error("SHA256 update failed");
    }
    if(!stream.eof()) throw std::runtime_error("hash file read failed");
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(handle.hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)
        throw std::runtime_error("SHA256 finish failed");
    constexpr char hex[]="0123456789abcdef"; std::string result;
    for(auto c:digest) { result+=hex[c>>4]; result+=hex[c&15]; }
    return result;
}
} // namespace asb::capture
