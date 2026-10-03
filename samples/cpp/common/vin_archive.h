#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef HVS_HAVE_ZSTD
#include <zstd.h>
#endif
namespace hv_archive {
inline constexpr char magic[8]={'H','V','Z','1','\r','\n',26,'\n'};
inline bool isArchive(const std::string& path){return std::filesystem::path(path).extension()==".zst";}
inline uint64_t hash(const uint8_t* p,size_t n){uint64_t h=14695981039346656037ULL;for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;}return h;}
inline uint64_t field(const std::string& s,const std::string& key) {
 auto p=s.find("\""+key+"\"");if(p==s.npos)throw std::runtime_error("Archive metadata missing "+key);
 p=s.find(':',p);if(p==s.npos)throw std::runtime_error("Invalid archive JSON");++p;
 while(p<s.size()&&s[p]==' ')++p;size_t end=p;while(end<s.size()&&s[end]>='0'&&s[end]<='9')++end;
 if(end==p)throw std::runtime_error("Invalid archive integer "+key);
 return std::stoull(s.substr(p,end-p));
}
struct Entry {uint64_t bytes=0,hostNs=0,frameId=0,width=0,height=0,stride=0,format=0,vinTimestamp=0,vinTvUs=0,archiveOffset=0,compressedBytes=0,offset=0;};
inline void checkLayout(const Entry& e,bool aps) {
 if(e.width!=(aps?1632:4096)||e.height!=(aps?1224:256)||e.stride<e.width*(aps?2:1)||e.stride>16384||
    e.bytes<e.stride*e.height||e.bytes>16*1024*1024||!e.compressedBytes||e.compressedBytes>20*1024*1024)
   throw std::runtime_error("Invalid/unsupported archive plane geometry or extent");
}
inline void metadata(std::ostream& out,const Entry& e,bool aps,uint64_t digest) {
 out<<"{\"stream\":\""<<(aps?"aps":"evs")<<"\",\"offset\":"<<e.offset<<",\"bytes\":"<<e.bytes
 <<",\"host_ns\":"<<e.hostNs<<",\"frame_id\":"<<e.frameId<<",\"width\":"<<e.width<<",\"height\":"<<e.height
 <<",\"stride\":"<<e.stride<<",\"format\":"<<e.format<<",\"vin_timestamp\":"<<e.vinTimestamp<<",\"vin_tv_us\":"<<e.vinTvUs
 <<",\"archive_offset\":"<<e.archiveOffset<<",\"archive_bytes\":"<<e.compressedBytes<<",\"fnv1a64\":\""<<std::hex<<digest<<std::dec
 <<"\",\"exposure_per_frame\":null,\"sensor_timestamp\":null}\n";
 if(!out)throw std::runtime_error("Archive metadata write failed");
}
class Reader {
 std::ifstream input_;std::vector<Entry> entries_;std::vector<uint8_t> packed_;bool aps_=false;
public:
 void open(const std::string& path) {
#ifndef HVS_HAVE_ZSTD
  throw std::runtime_error("This build has no zstd archive support; rebuild with libzstd development files");
#else
  entries_.clear();input_.close();input_.clear();input_.open(path,std::ios::binary);
  char signature[8]{};input_.read(signature,8);if(!input_||std::memcmp(signature,magic,8))throw std::runtime_error("Invalid HVZ1 archive header");
  aps_=std::filesystem::path(path).filename()=="aps.vin.zst";
  if(!aps_&&std::filesystem::path(path).filename()!="evs.vin.zst")throw std::runtime_error("Unknown archive stream filename");
  const auto size=std::filesystem::file_size(path);uint64_t end=8,rawOffset=0;
  std::ifstream meta(std::filesystem::path(path).parent_path()/"vin.frames.jsonl");
  if(!meta)throw std::runtime_error("Missing archive index vin.frames.jsonl");
  std::string line;const std::string tag=aps_?"\"aps\"":"\"evs\"";
  while(std::getline(meta,line)) {
   if(line.size()>8192)throw std::runtime_error("Oversized archive metadata row");
   if(line.find(tag)==line.npos)continue;
   Entry e;e.bytes=field(line,"bytes");e.hostNs=field(line,"host_ns");e.frameId=field(line,"frame_id");
   e.width=field(line,"width");e.height=field(line,"height");e.stride=field(line,"stride");e.format=field(line,"format");
   e.vinTimestamp=field(line,"vin_timestamp");e.vinTvUs=field(line,"vin_tv_us");e.offset=field(line,"offset");
   e.archiveOffset=field(line,"archive_offset");e.compressedBytes=field(line,"archive_bytes");checkLayout(e,aps_);
   if(e.archiveOffset!=end||e.offset!=rawOffset||end>size||e.compressedBytes>size-end||(!entries_.empty()&&e.hostNs<entries_.back().hostNs))
    throw std::runtime_error("Invalid archive offsets/size/timestamp order");
   end+=e.compressedBytes;rawOffset+=e.bytes;entries_.push_back(e);
   if(entries_.size()>1000000)throw std::runtime_error("Archive index exceeds supported frame count");
  }
  if(end!=size||entries_.empty())throw std::runtime_error("Truncated/extra/empty archive payload");
#endif
 }
 size_t size() const{return entries_.size();}
 const Entry& entry(size_t index)const{return entries_.at(index);}
 double fps()const{return entries_.size()>1&&entries_.back().hostNs>entries_.front().hostNs?(entries_.size()-1)*1e9/(entries_.back().hostNs-entries_.front().hostNs):30.;}
 void read(size_t index,std::vector<uint8_t>& output) {
#ifndef HVS_HAVE_ZSTD
  throw std::runtime_error("zstd support unavailable");
#else
  const auto& e=entries_.at(index);packed_.resize(size_t(e.compressedBytes));output.resize(size_t(e.bytes));
  input_.clear();input_.seekg(std::streamoff(e.archiveOffset));input_.read(reinterpret_cast<char*>(packed_.data()),std::streamsize(packed_.size()));
  if(!input_)throw std::runtime_error("Truncated archive frame");
  if(ZSTD_getFrameContentSize(packed_.data(),packed_.size())!=e.bytes)throw std::runtime_error("Archive frame size mismatch");
  const size_t result=ZSTD_decompress(output.data(),output.size(),packed_.data(),packed_.size());
  if(ZSTD_isError(result)||result!=output.size())throw std::runtime_error("Archive checksum/decompression failed");
#endif
 }
};
#ifdef HVS_HAVE_ZSTD
class Compressor {
 std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context_{ZSTD_createCCtx(),ZSTD_freeCCtx};
public:
 Compressor(){if(!context_)throw std::runtime_error("Cannot allocate zstd context");ZSTD_CCtx_setParameter(context_.get(),ZSTD_c_compressionLevel,1);ZSTD_CCtx_setParameter(context_.get(),ZSTD_c_checksumFlag,1);}
 size_t compress(const uint8_t* src,size_t n,std::vector<uint8_t>& out){out.resize(ZSTD_compressBound(n));const auto size=ZSTD_compress2(context_.get(),out.data(),out.size(),src,n);if(ZSTD_isError(size))throw std::runtime_error(ZSTD_getErrorName(size));return size;}
};
#endif
}
