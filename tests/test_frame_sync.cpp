#include "../samples/cpp/live_record_display/frame_sync.h"
#include <iostream>
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
int main() {
    using S=hv_live::FrameSync<int>;
    S s(10,20,3,2);
    s.push(false,{90,1}); s.push(true,{100,2});
    CHECK(!s.poll(119)); // wait for a successor, bounded even on EVS disconnect
    auto p=s.poll(120); CHECK(p && p->evs && p->deltaNs==-10); // inclusive tolerance
    s.push(true,{110,3}); s.push(false,{121,4});
    p=s.poll(110); CHECK(p && !p->evs); // both outside tolerance
    s.push(false,{130,5}); s.push(true,{126,6});
    p=s.poll(126); CHECK(p && p->evs && p->evs->value==5 && p->deltaNs==4);
    S missing(10,0); missing.push(true,{100,1});CHECK(!missing.poll(100)->evs);
    S empty(10,20);CHECK(!empty.poll(100000));
    S bounded(10,20,2,2);
    for(int i=0;i<10;++i) {bounded.push(false,{i,i}); bounded.push(true,{i,i});}
    auto st=bounded.stats();CHECK(st.evsDropped==8 && st.apsDropped==8);
    CHECK(bounded.poll(10)->aps.value==8); CHECK(bounded.poll(10)->aps.value==9);
    S tie(10,0); tie.push(false,{90,1});tie.push(false,{110,2});tie.push(true,{100,3});
    CHECK(tie.poll(100)->evs->value==1); // deterministic earlier on tie
    bool rejected=false;try{tie.push(false,{80,4});}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    std::cout<<"sync tolerance, nearest, timeout, missing stream, bounded queues and ties passed\n";
}
