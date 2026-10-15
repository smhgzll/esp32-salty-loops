// SPDX-License-Identifier: GPL-3.0-or-later
#include <string.h>
#include "check.hpp"
#include "sf2_fixture.hpp"
#include "daw/sf2_subset.hpp"
#include "daw/plugin_host.hpp"
using namespace daw;
int main()
{
    auto source=subsetFontFixture();
    size_t largestRead=0;
    Sf2Reader read=[&](uint32_t off,void *p,uint32_t n){ largestRead=std::max(largestRead,(size_t)n); if((uint64_t)off+n>source.size())return false;memcpy(p,source.data()+off,n);return true; };
    std::string err;Sf2Subset one,two;
    CHECK(sf2Subset(read,source.size(),{{"",0,5}},one,&err));
    CHECK(largestRead<4096); // planning reads tables, never the large PCM pool
    CHECK(one.pcmBytes<10000 && one.bytes()<source.size()/4);
    CHECK(sf2Subset(read,source.size(),{{"",0,5},{"",128,8}},two,&err));
    CHECK(two.pcmBytes==one.pcmBytes); // shared and linked samples are copied once
    std::vector<uint8_t> packed(two.bytes());
    CHECK(two.copy(read,packed.data(),packed.size(),&err));
    CHECK(!two.copy(read,packed.data(),packed.size()-1,&err));
    std::vector<Sf2Preset> presets;
    CHECK(sf2Presets([&](uint32_t o,void *p,uint32_t n){if((uint64_t)o+n>packed.size())return false;memcpy(p,packed.data()+o,n);return true;},packed.size(),presets,&err));
    CHECK(presets.size()==2 && presets[0].program==5 && presets[1].bank==128);
    auto original=LoadedResource::file("original.sf2",std::vector<uint8_t>(source));
    auto selected=LoadedResource::file("selected.sf2",std::move(packed));
    auto *d=PluginRegistry::find("org.smhgzll.soundfont");
    for(auto pick : std::vector<Sf2Preset>{{"",0,5},{"",128,8}}) {
        auto *a=PluginInstance::create(d,44100,256), *b=PluginInstance::create(d,44100,256);
        CHECK(a && b);PluginState state;state.setParam("bank",pick.bank);state.setParam("program",pick.program);
        a->applyState(state);b->applyState(state);CHECK(a->setResource(0,original));CHECK(b->setResource(0,selected));CHECK(a->activate() && b->activate());
        bool same=true;double energy=0;float al[256],ar[256],bl[256],br[256];float *oa[]={al,ar}, *ob[]={bl,br};
        for(int block=0;block<200;block++) {
            daw_event_t e{};e.type=block==150?DAW_EV_ALL_NOTES_OFF:DAW_EV_NOTE_ON;e.u.note.key=block==0?48:84;e.u.note.velocity=.8f;e.u.note.note_id=-1;
            daw_process_t ctx{};ctx.struct_size=sizeof ctx;ctx.frames=256;ctx.sample_pos=block*256;ctx.tempo_bpm=120;ctx.events=&e;ctx.event_count=(block==0||block==25||block==150)?1:0;
            ctx.outputs=oa;d->process(a->handle(),&ctx);ctx.outputs=ob;d->process(b->handle(),&ctx);
            same &= !memcmp(al,bl,sizeof al) && !memcmp(ar,br,sizeof ar);for(float x:al)energy+=x*x;
        }
        CHECK(same && energy>1);CHECK(b->memLarge()<a->memLarge());PluginInstance::destroy(a);PluginInstance::destroy(b);
    }
    Sf2Subset fallback;CHECK(sf2Subset(read,source.size(),{{"",1,127}},fallback,&err));
    auto broken=source;broken[4]=255;broken[5]=255;broken[6]=255;broken[7]=255;
    CHECK(!sf2Subset([&](uint32_t o,void *p,uint32_t n){if((uint64_t)o+n>broken.size())return false;memcpy(p,broken.data()+o,n);return true;},broken.size(),{{"",0,5}},fallback,&err));
    TEST_MAIN_END("test_sf2_subset");
}
