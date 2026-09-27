#include "../samples/cpp/hvs_record/avi_timing.h"
#include <chrono>
#include <iterator>
#include <string>
void u32(std::string& s,uint32_t n) { for(int i=0;i<4;++i) s.push_back(char(n>>(8*i))); }
std::string chunk(const std::string& tag,const std::string& data) {
    std::string s=tag;u32(s,uint32_t(data.size()));s+=data;if(data.size()%2)s+='\0';return s;
}
void require(bool b) {if(!b)throw std::runtime_error("AVI timing assertion");}
int main() {
    namespace fs=std::filesystem;
    auto path=fs::temp_directory_path()/("hvs-avi-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".avi");
    std::string stream(56,0); stream.replace(0,4,"vids");
    std::string body="AVI "+chunk("LIST","hdrl"+chunk("avih",std::string(56,0))+chunk("LIST","strl"+chunk("strh",stream)));
    body+=chunk("LIST","movi"+chunk("00db","avihstrh12345678"));
    auto original=chunk("RIFF",body);
    {std::ofstream f(path,std::ios::binary);f.write(original.data(),original.size());}
    setAviFrameRate(path,13.75);
    std::ifstream f(path,std::ios::binary);std::string result((std::istreambuf_iterator<char>(f)),{});f.close();
    auto read=[&](size_t at){uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(uint8_t(result[at+i]))<<(8*i);return n;};
    require(result.size()==original.size());
    require(read(result.find("avih")+8)==72727);
    auto at=result.find("strh")+8;
    require(read(at+20)==100000 && read(at+24)==1375000);
    require(result.substr(result.find("movi"))==original.substr(original.find("movi")));
    try {setAviFrameRate(path,0);return 1;}catch(const std::runtime_error&){}
    {std::ofstream bad(path,std::ios::binary|std::ios::trunc);bad<<"RIFF";}
    try {setAviFrameRate(path,13.75);return 2;}catch(const std::runtime_error&){}
    fs::remove(path);
}
