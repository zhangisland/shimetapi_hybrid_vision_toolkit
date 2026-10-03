#pragma once
// No recording writer is opened in this path. Latest-frame slots are bounded.
#include "../player/aps_color.h"
#include <opencv2/highgui.hpp>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <mutex>

static int previewVin(const Options& o) {
 cv::setNumThreads(2);
 std::signal(SIGINT,signalStop);std::signal(SIGTERM,signalStop);
 int lock=::open("/tmp/hvs-apx-capture.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) {if(lock>=0)::close(lock);throw std::runtime_error("Another capture owns the sensor");}
 struct Lock {int fd;~Lock(){::close(fd);}} lockOwner{lock};
 x5_capture* camera=nullptr;
 if(x5_open(&camera,o.profile,0,1,1)) throw std::runtime_error("Native preview open failed");
 std::unique_ptr<x5_capture,decltype(&x5_close)> owner(camera,x5_close);
 int addr=0,mode=0,index=0,bus=-1;
 if(x5_raw_identity(camera,&addr,&mode,&index,&bus)||addr!=o.address||mode!=2||index!=240)
  throw std::runtime_error("Unexpected native preview sensor mode");
 std::unique_ptr<SensorIO> sensor;apx::Registers regs;
 if(o.lines||o.gain>=0) {
  if(o.bus!=bus) throw std::runtime_error("Requested I2C bus differs from sensor configuration");
  sensor=std::make_unique<SensorIO>(bus,addr);regs=apx::apply(*sensor,o.lines,o.gain);
  std::cout<<"Preview requested lines="<<o.lines<<" gain_dB="<<o.gain<<" submitted_gain_dB="<<(o.gain>=0?apx::gainIndex(o.gain)*.375:-1)<<"; register configuration verified, not frame exposure"<<std::endl;
  for(auto r:regs) std::cout<<"register 0x"<<std::hex<<r.first<<"=0x"<<unsigned(r.second)<<std::dec<<std::endl;
 }
 struct Slot {std::mutex mutex;std::vector<uint8_t> bytes;int width=0,height=0,stride=0;uint64_t host=0,sequence=0;};
 Slot slots[2];std::atomic<bool> stop{false};std::atomic<uint64_t> counts[2]{},errors[2]{};
 std::string failures[2];
 // Two producer buffers plus one GUI snapshot per stream, no growing queue.
 slots[0].bytes.resize(1048576);slots[1].bytes.resize(3995136);
 auto receive=[&](int stream) {
  std::vector<uint8_t> back(slots[stream].bytes.size());
  while(!stop) {
   x5_image im{};int result=x5_get(camera,stream,&im);
   if(result) {++errors[stream];if(result==X5_CAPTURE_FATAL){failures[stream]="Fatal VIN get";stop=true;}continue;}
   try {
    const size_t expected=stream?3995136:1048576;
    if(!im.plane[0]||im.size[0]!=expected||im.width!=(stream?1632:4096)||im.height!=(stream?1224:256)||im.stride!=(stream?3264:4096))
     throw std::runtime_error("Unexpected VIN preview layout");
    std::memcpy(back.data(),im.plane[0],expected);
    const auto received=ns();
    {std::lock_guard<std::mutex> hold(slots[stream].mutex);auto& slot=slots[stream];slot.bytes.swap(back);slot.width=im.width;slot.height=im.height;slot.stride=im.stride;slot.host=received;++slot.sequence;}
    ++counts[stream];
   } catch(const std::exception& e){failures[stream]=e.what();stop=true;}
   if(x5_release(camera,stream,&im)){failures[stream]="VIN release failed";stop=true;}
  }
 };
 std::thread evs(receive,0),aps;
 try {aps=std::thread(receive,1);}catch(...){stop=true;evs.join();throw;}
 struct Join {std::atomic<bool>& stop;std::thread& a;std::thread& b;~Join(){stop=true;if(a.joinable())a.join();if(b.joinable())b.join();}} join{stop,evs,aps};
 hv_player::ApsIsp isp;isp.pattern=o.previewBayer;isp.previewScale=o.previewScale;
 Shimeta::codec::MipiRaw8Decoder decoder;
 const auto begin=ns();auto report=begin,verify=begin;uint64_t shown=0,last=0,lastEvs=0,dropped=0,lastSampleHost=begin;
 std::vector<uint8_t> raw(3995136),eventBytes(1048576),gray(1632*1224);
 double rawMean=0,rawSaturation=0,evsRate=0;uint64_t evsTotal=0,evsRateMark=0,evsRateTime=begin;
 cv::Mat eventView(608,768,CV_8UC3,cv::Scalar(0)),apsView;
 cv::namedWindow("HVS VIN preview - NO RECORDING",cv::WINDOW_NORMAL);
 cv::resizeWindow("HVS VIN preview - NO RECORDING",o.previewWidth,o.previewWidth/2);
 std::cout<<"Preview only: bounded latest frames, no output directory or payload files. q/ESC exits. APS display scale="<<o.previewScale<<std::endl;
 try {
 while(!stop&&!interrupted&&(!o.seconds||(ns()-begin)<o.seconds*1e9)) {
  uint64_t sequence=0,apsHost=0,evsHost=0;bool fresh=false;
  {std::lock_guard<std::mutex> hold(slots[1].mutex);sequence=slots[1].sequence;if(sequence&&sequence!=last){raw=slots[1].bytes;apsHost=slots[1].host;fresh=true;}}
  if(fresh) {
   if(last&&sequence>last+1)dropped+=sequence-last-1;last=sequence;lastSampleHost=apsHost;
   uint64_t saturated=0,sum=0;
   for(size_t i=0;i<gray.size();++i){const unsigned v=raw[2*i]|(unsigned(raw[2*i+1])<<8);gray[i]=uint8_t(std::min(v>>2,255u));if(i){sum+=v;saturated+=v>=1020&&v<=1023;}}
   rawMean=double(sum)/(gray.size()-1);rawSaturation=100.*saturated/(gray.size()-1);
   Shimeta::Frame f{};f.width=1632;f.height=1224;f.format=Shimeta::PixelFormat::Gray8;f.aps={gray.data(),gray.size()};apsView=isp.process(f);
   bool freshEvent=false;
   {std::lock_guard<std::mutex> hold(slots[0].mutex);evsHost=slots[0].host;if(slots[0].sequence!=lastEvs){eventBytes=slots[0].bytes;lastEvs=slots[0].sequence;freshEvent=true;}}
   if(freshEvent){std::vector<Shimeta::EventCD> events;decoder.Decode(eventBytes.data(),eventBytes.size(),events);evsTotal+=events.size();eventView.setTo(cv::Scalar(0));for(auto e:events)if(unsigned(e.x)<768&&unsigned(e.y)<608)eventView.at<cv::Vec3b>(e.y,e.x)=e.polarity?cv::Vec3b(255,0,0):cv::Vec3b(0,0,255);}
   const int w=o.previewWidth/2,h=w*3/4;cv::Mat canvas(h+80,w*2,CV_8UC3,cv::Scalar(18,18,18)),left,right;
   cv::resize(eventView,left,{w,h});cv::resize(apsView,right,{w,h});left.copyTo(canvas(cv::Rect(0,0,w,h)));right.copyTo(canvas(cv::Rect(w,0,w,h)));
   std::ostringstream title;title<<"NO RECORDING | RAW mean="<<double(sum)/(gray.size()-1)<<" sat="<<100.*saturated/(gray.size()-1)<<"% (first word excluded)";
   cv::putText(canvas,title.str(),{8,h+25},cv::FONT_HERSHEY_SIMPLEX,.42,{255,255,255},1);
   std::ostringstream timing;timing<<"Latest APS/EVS host delta="<<(int64_t(apsHost)-int64_t(evsHost))/1e6<<" ms; EVS events/s="<<uint64_t(evsRate);
   cv::putText(canvas,timing.str(),{8,h+50},cv::FONT_HERSHEY_SIMPLEX,.42,{255,255,255},1);
   cv::imshow("HVS VIN preview - NO RECORDING",canvas);++shown;
  }
  if(o.verifyRegs&&ns()-verify>=1000000000ULL){if(sensor)apx::verify(*sensor,regs);verify=ns();}
  if(ns()-report>=1000000000ULL){const auto tick=ns();evsRate=double(evsTotal-evsRateMark)*1e9/double(tick-evsRateTime);evsRateMark=evsTotal;evsRateTime=tick;std::cout<<"Preview APS received="<<counts[1]<<" EVS packets="<<counts[0]<<" displayed="<<shown<<" superseded APS previews="<<dropped<<" raw_mean="<<rawMean<<" raw_sat_percent="<<rawSaturation<<" evs_events_per_s="<<uint64_t(evsRate)<<" telemetry_ns="<<tick<<" raw_sample_sequence="<<last<<" raw_sample_age_ms="<<double(tick-lastSampleHost)/1e6<<" evs_event_scope=displayed_packets_only"<<std::endl;report=ns();}
  if(ns()-begin>uint64_t(o.timeout*1e9)&&(!counts[0]||!counts[1]))throw std::runtime_error("Preview stream absent");
  const int key=cv::waitKey(1)&255;if(key==27||key=='q')break;
 }
 } catch(...){cv::destroyAllWindows();throw;}
 stop=true;evs.join();aps.join();if(o.verifyRegs&&sensor)apx::verify(*sensor,regs);cv::destroyAllWindows();
 for(const auto& error:failures)if(!error.empty())throw std::runtime_error(error);
 std::cout<<"Preview stopped: APS="<<counts[1]<<" EVS packets="<<counts[0]<<" get errors="<<errors[1]<<'/'<<errors[0]<<"; no recording saved"<<std::endl;
 return 0;
}
