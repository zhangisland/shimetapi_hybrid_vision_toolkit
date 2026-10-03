#pragma once
#include "../common/vin_archive.h"
static void saveFast(const Options& o,const MemoryArena& arena,double fps,const std::string& report) {
#ifndef HVS_HAVE_ZSTD
 throw std::runtime_error("Fast save requires zstd-enabled build");
#else
 const auto begin=ns();const fs::path stage=o.output/"saving.partial";fs::create_directories(stage);
 // Check a conservative incompressible bound, not an assumed compression ratio.
 if(fs::space(stage).available<arena.used()+arena.used()/64+(32ULL<<20))throw std::runtime_error("Insufficient free space for lossless archive; free space and retry");
 hv_archive::Compressor compressor;std::vector<uint8_t> packed;
 std::ofstream meta(stage/"vin.frames.jsonl",std::ios::trunc);
 if(!meta)throw std::runtime_error("Cannot open archive metadata");
 uint64_t counts[2]{},offsets[2]{},compressed=0,compressNs=0,writeNs=0,verifyNs=0,syncNs=0,done=0;
 for(int stream=0;stream<2;++stream) {
  const auto name=stream?"aps.vin.zst":"evs.vin.zst";
  std::ofstream out(stage/name,std::ios::binary|std::ios::trunc);out.write(hv_archive::magic,8);uint64_t fileOffset=8;
  arena.each([&](const MemoryArena::Header& h,const uint8_t* bytes) {
   if(int(h.stream)!=stream)return;
   const auto c=ns();const auto n=compressor.compress(bytes,h.bytes,packed);compressNs+=ns()-c;
   hv_archive::Entry e{h.bytes,h.hostNs,h.frameId,h.width,h.height,h.stride,h.format,h.vinTimestamp,h.vinTvUs,fileOffset,n,offsets[stream]};
   hv_archive::checkLayout(e,stream!=0);
   const auto w=ns();out.write(reinterpret_cast<const char*>(packed.data()),std::streamsize(n));
   hv_archive::metadata(meta,e,stream!=0,hv_archive::hash(bytes,h.bytes));writeNs+=ns()-w;
   offsets[stream]+=h.bytes;fileOffset+=n;compressed+=n;++counts[stream];done+=MemoryArena::extent(h.bytes);
   if(counts[stream]%30==0)std::cout<<"Compressing "<<done*100/std::max<size_t>(1,arena.used())<<"% "<<name<<std::endl;
  });
  checkedClose(out);const auto sync=ns();RecordingStorage::syncFile(stage/name);syncNs+=ns()-sync;
 }
 checkedClose(meta);
 const auto verifying=ns();
 {
  hv_archive::Reader readers[2];readers[0].open((stage/"evs.vin.zst").string());readers[1].open((stage/"aps.vin.zst").string());
  size_t at[2]{};std::vector<uint8_t> restored;
  arena.each([&](const MemoryArena::Header& h,const uint8_t* bytes){readers[h.stream].read(at[h.stream]++,restored);if(restored.size()!=h.bytes||std::memcmp(restored.data(),bytes,h.bytes))throw std::runtime_error("Lossless archive byte verification failed");});
 }
 verifyNs+=ns()-verifying;RecordingStorage::syncFile(stage/"vin.frames.jsonl");
 std::ofstream summary(stage/"summary.txt",std::ios::trunc);
 summary<<report<<"session_format=vin_zstd_v1\naps_frames="<<counts[1]<<"\nevs_packets="<<counts[0]
 <<"\naps_observed_fps="<<fps<<"\naps_width=1632\naps_height=1224\nevs_width=768\nevs_height=608"
 <<"\naps_storage_format=RAW10_16LE_with_stride_zstd\ngray8_frames=0\naps_paired_timestamps=0"
 <<"\narchive_checksum=zstd_frame_checksum_and_full_byte_compare\narchive_level=1\narchive_payload_bytes="<<compressed
 <<"\narchive_original_bytes="<<(offsets[0]+offsets[1])<<"\ncompress_ns="<<compressNs<<"\nwrite_ns="<<writeNs
 <<"\nverify_ns="<<verifyNs<<"\nsync_ns="<<syncNs<<"\nsave_seconds="<<(ns()-begin)/1e9<<'\n';
 checkedClose(summary);RecordingStorage::syncFile(stage/"summary.txt");
 for(const char* name:{"aps.vin.zst","evs.vin.zst","vin.frames.jsonl"}){if(fs::exists(o.output/name))fs::remove(o.output/name);fs::rename(stage/name,o.output/name);}
 RecordingStorage::syncFile(o.output);
 if(fs::exists(o.output/"summary.txt"))fs::remove(o.output/"summary.txt");
 fs::rename(stage/"summary.txt",o.output/"summary.txt");
 try{RecordingStorage::syncFile(o.output);}catch(...){fs::rename(o.output/"summary.txt",stage/"summary.txt");throw;}
 fs::remove(o.output/"recording.storage");
 std::cout<<"Lossless archive saved and byte-verified in "<<(ns()-begin)/1e9<<" seconds; original bytes="<<(offsets[0]+offsets[1])<<" stored bytes="<<compressed<<std::endl;
#endif
}
static int exportFast(const Options& o) {
 std::ifstream summary(o.exportSession/"summary.txt");std::string report((std::istreambuf_iterator<char>(summary)),{});
 std::string status,format,line;std::istringstream fields(report);while(std::getline(fields,line)){if(line.rfind("status=",0)==0)status=line.substr(7);if(line.rfind("session_format=",0)==0)format=line.substr(15);}
 if(status!="complete"||format!="vin_zstd_v1")
  throw std::runtime_error("Export requires a finalized vin_zstd_v1 session");
 hv_archive::Reader readers[2];readers[0].open((o.exportSession/"evs.vin.zst").string());readers[1].open((o.exportSession/"aps.vin.zst").string());
 const size_t capacity=MemoryArena::payloadCapacity(o.maxBytes,RecordingStorage::availableMemory());
 size_t needed=0;for(auto& reader:readers)for(size_t i=0;i<reader.size();++i){const auto n=MemoryArena::extent(reader.entry(i).bytes);if(n>capacity-needed)throw std::runtime_error("Export memory budget too small; increase --max-mib to original recording budget");needed+=n;}
 MemoryArena arena(needed);std::vector<uint8_t> bytes;
 for(int stream=0;stream<2;++stream)for(size_t i=0;i<readers[stream].size();++i){const auto& e=readers[stream].entry(i);readers[stream].read(i,bytes);MemoryArena::Header h{e.bytes,e.hostNs,0,0,uint32_t(stream),uint32_t(e.width),uint32_t(e.height),uint32_t(e.stride),uint32_t(e.frameId),uint32_t(e.format),e.vinTimestamp,e.vinTvUs};if(!arena.append(h,bytes.data()))throw std::runtime_error("Export capacity mismatch");}
 if(!fs::create_directory(o.output))throw std::runtime_error("Export output must be a new directory");
 std::ofstream(o.output/"recording.storage")<<"export in progress; source archive retained\n";
 save(o,arena,readers[1].fps(),report);
 std::cout<<"Export complete. Source archive retained unchanged.\n";return 0;
}
