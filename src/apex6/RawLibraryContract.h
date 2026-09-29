#pragma once
#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace asb::apex6 {
// asb8 and asb9 both advertised ABI 1 / capability bits 7. The build record
// binds the required authoritative requested-packet-length contract to the DLL.
inline bool matchesRawLibraryContract(const std::string& record,std::string hash) {
    if(record.size()>16384||hash.size()!=64)return false;
    std::smatch version,artifact;
    if(!std::regex_search(record,version,std::regex("Integrated library version: v0\\.7\\.0-asb([0-9]+)\\r?\\n"))||
       !std::regex_search(record,artifact,std::regex("Artifact SHA-256: ([0-9a-fA-F]{64})\\r?\\n")))return false;
    unsigned revision=0;try{revision=static_cast<unsigned>(std::stoul(version[1].str()));}catch(...){return false;}
    if(revision<9)return false;
    auto expected=artifact[1].str();
    auto lower=[](unsigned char c){return static_cast<char>(std::tolower(c));};
    std::transform(hash.begin(),hash.end(),hash.begin(),lower);std::transform(expected.begin(),expected.end(),expected.begin(),lower);
    return hash==expected;
}
}
